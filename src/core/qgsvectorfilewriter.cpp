/***************************************************************************
                          qgsvectorfilewriter.cpp
                          generic vector file writer
                             -------------------
    begin                : Sat Jun 16 2004
    copyright            : (C) 2004 by Tim Sutton
    email                : tim at linfiniti.com
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include "qgsapplication.h"
#include "qgsfield.h"
#include "qgsfeature.h"
#include "qgsfeatureiterator.h"
#include "qgsgeometry.h"
#include "qgslogger.h"
#include "qgsmessagelog.h"
#include "qgscoordinatereferencesystem.h"
#include "qgsvectorfilewriter.h"
#include "qgsrenderer.h"
#include "qgssymbollayer.h"
#include "qgsvectordataprovider.h"
#include "qgsvectorlayer.h"
#include "qgslocalec.h"
#include "qgscsexception.h"

#include <QFile>
#include <QSettings>
#include <QFileInfo>
#include <QDir>
#include <QTextCodec>
#include <QTextStream>
#include <QSet>
#include <QMetaType>

#include <cassert>
#include <cstdlib> // size_t
#include <limits> // std::numeric_limits

#include <ogr_srs_api.h>
#include <cpl_error.h>
#include <cpl_conv.h>
#include <gdal.h>

#if defined(GDAL_VERSION_NUM) && GDAL_VERSION_NUM >= 1800
#define TO8F(x)  (x).toUtf8().constData()
#else
#define TO8F(x)  QFile::encodeName( x ).constData()
#endif

QgsVectorFileWriter::FieldValueConverter::FieldValueConverter()
{
}

QgsVectorFileWriter::FieldValueConverter::~FieldValueConverter()
{
}

QgsField QgsVectorFileWriter::FieldValueConverter::fieldDefinition( const QgsField& field )
{
  return field;
}

QVariant QgsVectorFileWriter::FieldValueConverter::convert( int /*fieldIdxInLayer*/, const QVariant& value )
{
  return value;
}

QgsVectorFileWriter::QgsVectorFileWriter(
  const QString &theVectorFileName,
  const QString &theFileEncoding,
  const QgsFields& fields,
  QgsWkbTypes::Type geometryType,
  const QgsCoordinateReferenceSystem& srs,
  const QString& driverName,
  const QStringList &datasourceOptions,
  const QStringList &layerOptions,
  QString *newFilename,
  SymbologyExport symbologyExport
)
    : mDS( nullptr )
    , mLayer( nullptr )
    , mOgrRef( nullptr )
    , mGeom( nullptr )
    , mError( NoError )
    , mCodec( nullptr )
    , mWkbType( geometryType )
    , mSymbologyExport( symbologyExport )
    , mSymbologyScaleDenominator( 1.0 )
    , mFieldValueConverter( nullptr )
{
  init( theVectorFileName, theFileEncoding, fields,  geometryType ,
        srs, driverName, datasourceOptions, layerOptions, newFilename, nullptr );
}

QgsVectorFileWriter::QgsVectorFileWriter( const QString& vectorFileName,
    const QString& fileEncoding,
    const QgsFields& fields,
    QgsWkbTypes::Type geometryType,
    const QgsCoordinateReferenceSystem& srs,
    const QString& driverName,
    const QStringList& datasourceOptions,
    const QStringList& layerOptions,
    QString* newFilename,
    QgsVectorFileWriter::SymbologyExport symbologyExport,
    FieldValueConverter* fieldValueConverter )
    : mDS( nullptr )
    , mLayer( nullptr )
    , mOgrRef( nullptr )
    , mGeom( nullptr )
    , mError( NoError )
    , mCodec( nullptr )
    , mWkbType( geometryType )
    , mSymbologyExport( symbologyExport )
    , mSymbologyScaleDenominator( 1.0 )
    , mFieldValueConverter( nullptr )
{
  init( vectorFileName, fileEncoding, fields, geometryType, srs, driverName,
        datasourceOptions, layerOptions, newFilename, fieldValueConverter );
}

void QgsVectorFileWriter::init( QString vectorFileName,
                                QString fileEncoding,
                                const QgsFields& fields,
                                QgsWkbTypes::Type geometryType,
                                QgsCoordinateReferenceSystem srs,
                                const QString& driverName,
                                QStringList datasourceOptions,
                                QStringList layerOptions,
                                QString* newFilename,
                                FieldValueConverter* fieldValueConverter )
{
  mRenderContext.setRendererScale( mSymbologyScaleDenominator );

  if ( vectorFileName.isEmpty() )
  {
    mErrorMessage = QObject::tr( "Empty filename given" );
    mError = ErrCreateDataSource;
    return;
  }

  if ( driverName == "MapInfo MIF" )
  {
    mOgrDriverName = "MapInfo File";
  }
  else if ( driverName == "SpatiaLite" )
  {
    mOgrDriverName = "SQLite";
    if ( !datasourceOptions.contains( "SPATIALITE=YES" ) )
    {
      datasourceOptions.append( "SPATIALITE=YES" );
    }
  }
  else if ( driverName == "DBF file" )
  {
    mOgrDriverName = "ESRI Shapefile";
    if ( !layerOptions.contains( "SHPT=NULL" ) )
    {
      layerOptions.append( "SHPT=NULL" );
    }
    srs = QgsCoordinateReferenceSystem();
  }
  else
  {
    mOgrDriverName = driverName;
  }

  // find driver in OGR
  OGRSFDriverH poDriver;
  QgsApplication::registerOgrDrivers();

  poDriver = OGRGetDriverByName( mOgrDriverName.toLocal8Bit().constData() );

  if ( !poDriver )
  {
    mErrorMessage = QObject::tr( "OGR driver for '%1' not found (OGR error: %2)" )
                    .arg( driverName,
                          QString::fromUtf8( CPLGetLastErrorMsg() ) );
    mError = ErrDriverNotFound;
    return;
  }

  MetaData metadata;
  bool metadataFound = driverMetadata( driverName, metadata );

  if ( mOgrDriverName == "ESRI Shapefile" )
  {
    if ( layerOptions.join( "" ).toUpper().indexOf( "ENCODING=" ) == -1 )
    {
      layerOptions.append( "ENCODING=" + convertCodecNameForEncodingOption( fileEncoding ) );
    }

    if ( driverName == "ESRI Shapefile" && !vectorFileName.endsWith( ".shp", Qt::CaseInsensitive ) )
    {
      vectorFileName += ".shp";
    }
    else if ( driverName == "DBF file" && !vectorFileName.endsWith( ".dbf", Qt::CaseInsensitive ) )
    {
      vectorFileName += ".dbf";
    }

#if defined(GDAL_VERSION_NUM) && GDAL_VERSION_NUM < 1700
    // check for unique fieldnames
    QSet<QString> fieldNames;
    for ( int i = 0; i < fields.count(); ++i )
    {
      QString name = fields[i].name().left( 10 );
      if ( fieldNames.contains( name ) )
      {
        mErrorMessage = QObject::tr( "trimming attribute name '%1' to ten significant characters produces duplicate column name." )
                        .arg( fields[i].name() );
        mError = ErrAttributeCreationFailed;
        return;
      }
      fieldNames << name;
    }
#endif

    deleteShapeFile( vectorFileName );
  }
  else
  {
    if ( metadataFound )
    {
      QStringList allExts = metadata.ext.split( ' ', QString::SkipEmptyParts );
      bool found = false;
      Q_FOREACH ( const QString& ext, allExts )
      {
        if ( vectorFileName.endsWith( '.' + ext, Qt::CaseInsensitive ) )
        {
          found = true;
          break;
        }
      }

      if ( !found )
      {
        vectorFileName += '.' + allExts[0];
      }
    }

    QFile::remove( vectorFileName );
  }

  if ( metadataFound && !metadata.compulsoryEncoding.isEmpty() )
  {
    if ( fileEncoding.compare( metadata.compulsoryEncoding, Qt::CaseInsensitive ) != 0 )
    {
      QgsDebugMsg( QString( "forced %1 encoding for %2" ).arg( metadata.compulsoryEncoding ).arg( driverName ) );
      fileEncoding = metadata.compulsoryEncoding;
    }

  }

  char **options = nullptr;
  if ( !datasourceOptions.isEmpty() )
  {
    options = new char *[ datasourceOptions.size()+1 ];
    for ( int i = 0; i < datasourceOptions.size(); i++ )
    {
      options[i] = CPLStrdup( datasourceOptions[i].toLocal8Bit().constData() );
    }
    options[ datasourceOptions.size()] = nullptr;
  }

  // create the data source
  mDS = OGR_Dr_CreateDataSource( poDriver, TO8F( vectorFileName ), options );

  if ( options )
  {
    for ( int i = 0; i < datasourceOptions.size(); i++ )
      CPLFree( options[i] );
    delete [] options;
    options = nullptr;
  }

  if ( !mDS )
  {
    mError = ErrCreateDataSource;
    mErrorMessage = QObject::tr( "creation of data source failed (OGR error:%1)" )
                    .arg( QString::fromUtf8( CPLGetLastErrorMsg() ) );
    return;
  }

  QgsDebugMsg( "Created data source" );

  // use appropriate codec
  mCodec = QTextCodec::codecForName( fileEncoding.toLocal8Bit().constData() );
  if ( !mCodec )
  {
    QgsDebugMsg( "error finding QTextCodec for " + fileEncoding );

    QSettings settings;
    QString enc = settings.value( "/UI/encoding", "System" ).toString();
    mCodec = QTextCodec::codecForName( enc.toLocal8Bit().constData() );
    if ( !mCodec )
    {
      QgsDebugMsg( "error finding QTextCodec for " + enc );
      mCodec = QTextCodec::codecForLocale();
      Q_ASSERT( mCodec );
    }
  }

  // consider spatial reference system of the layer
  if ( srs.isValid() )
  {
    QString srsWkt = srs.toWkt();
    QgsDebugMsg( "WKT to save as is " + srsWkt );
    mOgrRef = OSRNewSpatialReference( srsWkt.toLocal8Bit().constData() );
  }

  // datasource created, now create the output layer
  QString layerName = QFileInfo( vectorFileName ).baseName();
  OGRwkbGeometryType wkbType = ogrTypeFromWkbType( geometryType );

  // Remove FEATURE_DATASET layer option (used for ESRI File GDB driver) if its value is not set
  int optIndex = layerOptions.indexOf( "FEATURE_DATASET=" );
  if ( optIndex != -1 )
  {
    layerOptions.removeAt( optIndex );
  }

  if ( !layerOptions.isEmpty() )
  {
    options = new char *[ layerOptions.size()+1 ];
    for ( int i = 0; i < layerOptions.size(); i++ )
    {
      options[i] = CPLStrdup( layerOptions[i].toLocal8Bit().constData() );
    }
    options[ layerOptions.size()] = nullptr;
  }

  // disable encoding conversion of OGR Shapefile layer
  CPLSetConfigOption( "SHAPE_ENCODING", "" );

  mLayer = OGR_DS_CreateLayer( mDS, TO8F( layerName ), mOgrRef, wkbType, options );

  if ( options )
  {
    for ( int i = 0; i < layerOptions.size(); i++ )
      CPLFree( options[i] );
    delete [] options;
    options = nullptr;
  }

  QSettings settings;
  if ( !settings.value( "/qgis/ignoreShapeEncoding", true ).toBool() )
  {
    CPLSetConfigOption( "SHAPE_ENCODING", nullptr );
  }

  if ( srs.isValid() )
  {
    if ( mOgrDriverName == "ESRI Shapefile" )
    {
      QString layerName = vectorFileName.left( vectorFileName.indexOf( ".shp", Qt::CaseInsensitive ) );
      QFile prjFile( layerName + ".qpj" );
      if ( prjFile.open( QIODevice::WriteOnly  | QIODevice::Truncate ) )
      {
        QTextStream prjStream( &prjFile );
        prjStream << srs.toWkt().toLocal8Bit().constData() << endl;
        prjFile.close();
      }
      else
      {
        QgsDebugMsg( "Couldn't open file " + layerName + ".qpj" );
      }
    }
  }

  if ( !mLayer )
  {
    mErrorMessage = QObject::tr( "creation of layer failed (OGR error:%1)" )
                    .arg( QString::fromUtf8( CPLGetLastErrorMsg() ) );
    mError = ErrCreateLayer;
    return;
  }

  OGRFeatureDefnH defn = OGR_L_GetLayerDefn( mLayer );

  QgsDebugMsg( "created layer" );

  // create the fields
  QgsDebugMsg( "creating " + QString::number( fields.size() ) + " fields" );

  mFields = fields;
  mAttrIdxToOgrIdx.clear();
  QSet<int> existingIdxs;

  mFieldValueConverter = fieldValueConverter;

  for ( int fldIdx = 0; fldIdx < fields.count(); ++fldIdx )
  {
    QgsField attrField = fields.at( fldIdx );

    OGRFieldType ogrType = OFTString; //default to string

    if ( fieldValueConverter )
    {
      attrField = fieldValueConverter->fieldDefinition( fields.at( fldIdx ) );
    }

    int ogrWidth = attrField.length();
    int ogrPrecision = attrField.precision();
    if ( ogrPrecision > 0 )
      ++ogrWidth;

    switch ( attrField.type() )
    {
#if defined(GDAL_VERSION_NUM) && GDAL_VERSION_NUM < 2000000
      case QVariant::LongLong:
        ogrType = OFTString;
        ogrWidth = ogrWidth > 0 && ogrWidth <= 21 ? ogrWidth : 21;
        ogrPrecision = -1;
        break;
#else
      case QVariant::LongLong:
      {
        const char* pszDataTypes = GDALGetMetadataItem( poDriver, GDAL_DMD_CREATIONFIELDDATATYPES, NULL );
        if ( pszDataTypes && strstr( pszDataTypes, "Integer64" ) )
          ogrType = OFTInteger64;
        else
          ogrType = OFTReal;
        ogrWidth = ogrWidth > 0 && ogrWidth <= 20 ? ogrWidth : 20;
        ogrPrecision = 0;
        break;
      }
#endif
      case QVariant::String:
        ogrType = OFTString;
        if ( ogrWidth <= 0 || ogrWidth > 255 )
          ogrWidth = 255;
        break;

      case QVariant::Int:
        ogrType = OFTInteger;
        ogrWidth = ogrWidth > 0 && ogrWidth <= 10 ? ogrWidth : 10;
        ogrPrecision = 0;
        break;

      case QVariant::Double:
        ogrType = OFTReal;
        break;

      case QVariant::Date:
        ogrType = OFTDate;
        break;

      case QVariant::Time:
        if ( mOgrDriverName == "ESRI Shapefile" )
        {
          ogrType = OFTString;
          ogrWidth = 12; // %02d:%02d:%06.3f
        }
        else
        {
          ogrType = OFTTime;
        }
        break;

      case QVariant::DateTime:
        if ( mOgrDriverName == "ESRI Shapefile" )
        {
          ogrType = OFTString;
          ogrWidth = 24; // "%04d/%02d/%02d %02d:%02d:%06.3f"
        }
        else
        {
          ogrType = OFTDateTime;
        }
        break;

      default:
        //assert(0 && "invalid variant type!");
        mErrorMessage = QObject::tr( "unsupported type for field %1" )
                        .arg( attrField.name() );
        mError = ErrAttributeTypeUnsupported;
        return;
    }

    QString name( attrField.name() );

    if ( mOgrDriverName == "SQLite" && name.compare( "ogc_fid", Qt::CaseInsensitive ) == 0 )
    {
      int i;
      for ( i = 0; i < 10; i++ )
      {
        name = QString( "ogc_fid%1" ).arg( i );

        int j;
        for ( j = 0; j < fields.size() && name.compare( fields.at( j ).name(), Qt::CaseInsensitive ) != 0; j++ )
          ;

        if ( j == fields.size() )
          break;
      }

      if ( i == 10 )
      {
        mErrorMessage = QObject::tr( "no available replacement for internal fieldname ogc_fid found" ).arg( attrField.name() );
        mError = ErrAttributeCreationFailed;
        return;
      }

      QgsMessageLog::logMessage( QObject::tr( "Reserved attribute name ogc_fid replaced with %1" ).arg( name ), QObject::tr( "OGR" ) );
    }

    // create field definition
    OGRFieldDefnH fld = OGR_Fld_Create( mCodec->fromUnicode( name ), ogrType );
    if ( ogrWidth > 0 )
    {
      OGR_Fld_SetWidth( fld, ogrWidth );
    }

    if ( ogrPrecision >= 0 )
    {
      OGR_Fld_SetPrecision( fld, ogrPrecision );
    }

    // create the field
    QgsDebugMsg( "creating field " + attrField.name() +
                 " type " + QString( QVariant::typeToName( attrField.type() ) ) +
                 " width " + QString::number( ogrWidth ) +
                 " precision " + QString::number( ogrPrecision ) );
    if ( OGR_L_CreateField( mLayer, fld, true ) != OGRERR_NONE )
    {
      QgsDebugMsg( "error creating field " + attrField.name() );
      mErrorMessage = QObject::tr( "creation of field %1 failed (OGR error: %2)" )
                      .arg( attrField.name(),
                            QString::fromUtf8( CPLGetLastErrorMsg() ) );
      mError = ErrAttributeCreationFailed;
      OGR_Fld_Destroy( fld );
      return;
    }
    OGR_Fld_Destroy( fld );

    int ogrIdx = OGR_FD_GetFieldIndex( defn, mCodec->fromUnicode( name ) );
    QgsDebugMsg( QString( "returned field index for %1: %2" ).arg( name ).arg( ogrIdx ) );
    if ( ogrIdx < 0 || existingIdxs.contains( ogrIdx ) )
    {
#if defined(GDAL_VERSION_NUM) && GDAL_VERSION_NUM < 1700
      // if we didn't find our new column, assume it's name was truncated and
      // it was the last one added (like for shape files)
      int fieldCount = OGR_FD_GetFieldCount( defn );

      OGRFieldDefnH fdefn = OGR_FD_GetFieldDefn( defn, fieldCount - 1 );
      if ( fdefn )
      {
        const char *fieldName = OGR_Fld_GetNameRef( fdefn );

        if ( attrField.name().left( strlen( fieldName ) ) == fieldName )
        {
          ogrIdx = fieldCount - 1;
        }
      }
#else
      // GDAL 1.7 not just truncates, but launders more aggressivly.
      ogrIdx = OGR_FD_GetFieldCount( defn ) - 1;
#endif

      if ( ogrIdx < 0 )
      {
        QgsDebugMsg( "error creating field " + attrField.name() );
        mErrorMessage = QObject::tr( "created field %1 not found (OGR error: %2)" )
                        .arg( attrField.name(),
                              QString::fromUtf8( CPLGetLastErrorMsg() ) );
        mError = ErrAttributeCreationFailed;
        return;
      }
    }

    existingIdxs.insert( ogrIdx );
    mAttrIdxToOgrIdx.insert( fldIdx, ogrIdx );
  }

  QgsDebugMsg( "Done creating fields" );

  mWkbType = geometryType;
  if ( mWkbType != QgsWkbTypes::NoGeometry )
  {
    // create geometry which will be used for import
    mGeom = createEmptyGeometry( mWkbType );
  }

  if ( newFilename )
    *newFilename = vectorFileName;
}

OGRGeometryH QgsVectorFileWriter::createEmptyGeometry( QgsWkbTypes::Type wkbType )
{
  return OGR_G_CreateGeometry( ogrTypeFromWkbType( wkbType ) );
}

QMap<QString, QgsVectorFileWriter::MetaData> QgsVectorFileWriter::initMetaData()
{
  QMap<QString, MetaData> driverMetadata;

  QMap<QString, Option*> datasetOptions;
  QMap<QString, Option*> layerOptions;

  // Arc/Info ASCII Coverage
  datasetOptions.clear();
  layerOptions.clear();

  driverMetadata.insert( "AVCE00",
                         MetaData(
                           "Arc/Info ASCII Coverage",
                           QObject::tr( "Arc/Info ASCII Coverage" ),
                           "*.e00",
                           "e00",
                           datasetOptions,
                           layerOptions
                         )
                       );

  // Atlas BNA
  datasetOptions.clear();
  layerOptions.clear();

  datasetOptions.insert( "LINEFORMAT", new SetOption(
                           QObject::tr( "New BNA files are created by the "
                                        "systems default line termination conventions. "
                                        "This may be overridden here." ),
                           QStringList()
                           << "CRLF"
                           << "LF",
                           "", // Default value
                           true // Allow None
                         ) );

  datasetOptions.insert( "MULTILINE", new BoolOption(
                           QObject::tr( "By default, BNA files are created in multi-line format. "
                                        "For each record, the first line contains the identifiers and the "
                                        "type/number of coordinates to follow. Each following line contains "
                                        "a pair of coordinates." ),
                           true  // Default value
                         ) );

  datasetOptions.insert( "NB_IDS", new SetOption(
                           QObject::tr( "BNA records may contain from 2 to 4 identifiers per record. "
                                        "Some software packages only support a precise number of identifiers. "
                                        "You can override the default value (2) by a precise value" ),
                           QStringList()
                           << "2"
                           << "3"
                           << "4"
                           << "NB_SOURCE_FIELDS",
                           "2" // Default value
                         ) );

  datasetOptions.insert( "ELLIPSES_AS_ELLIPSES", new BoolOption(
                           QObject::tr( "The BNA writer will try to recognize ellipses and circles when writing a polygon. "
                                        "This will only work if the feature has previously been read from a BNA file. "
                                        "As some software packages do not support ellipses/circles in BNA data file, "
                                        "it may be useful to tell the writer by specifying ELLIPSES_AS_ELLIPSES=NO not "
                                        "to export them as such, but keep them as polygons." ),
                           true  // Default value
                         ) );

  datasetOptions.insert( "NB_PAIRS_PER_LINE", new IntOption(
                           QObject::tr( "Limit the number of coordinate pairs per line in multiline format." ),
                           2 // Default value
                         ) );

  datasetOptions.insert( "COORDINATE_PRECISION", new IntOption(
                           QObject::tr( "Set the number of decimal for coordinates. Default value is 10." ),
                           10 // Default value
                         ) );

  driverMetadata.insert( "BNA",
                         MetaData(
                           "Atlas BNA",
                           QObject::tr( "Atlas BNA" ),
                           "*.bna",
                           "bna",
                           datasetOptions,
                           layerOptions
                         )
                       );

  // Comma Separated Value
  datasetOptions.clear();
  layerOptions.clear();

  layerOptions.insert( "LINEFORMAT", new SetOption(
                         QObject::tr( "By default when creating new .csv files they "
                                      "are created with the line termination conventions "
                                      "of the local platform (CR/LF on Win32 or LF on all other systems). "
                                      "This may be overridden through the use of the LINEFORMAT option." ),
                         QStringList()
                         << "CRLF"
                         << "LF",
                         "", // Default value
                         true // Allow None
                       ) );

  layerOptions.insert( "GEOMETRY", new SetOption(
                         QObject::tr( "By default, the geometry of a feature written to a .csv file is discarded. "
                                      "It is possible to export the geometry in its WKT representation by "
                                      "specifying GEOMETRY=AS_WKT. It is also possible to export point geometries "
                                      "into their X,Y,Z components by specifying GEOMETRY=AS_XYZ, GEOMETRY=AS_XY "
                                      "or GEOMETRY=AS_YX." ),
                         QStringList()
                         << "AS_WKT"
                         << "AS_XYZ"
                         << "AS_XY"
                         << "AS_YX",
                         "AS_XY", // Default value
                         true // Allow None
                       ) );

  layerOptions.insert( "CREATE_CSVT", new BoolOption(
                         QObject::tr( "Create the associated .csvt file to describe the type of each "
                                      "column of the layer and its optional width and precision." ),
                         false  // Default value
                       ) );

  layerOptions.insert( "SEPARATOR", new SetOption(
                         QObject::tr( "Field separator character." ),
                         QStringList()
                         << "COMMA"
                         << "SEMICOLON"
                         << "TAB",
                         "COMMA" // Default value
                       ) );

  layerOptions.insert( "WRITE_BOM", new BoolOption(
                         QObject::tr( "Write a UTF-8 Byte Order Mark (BOM) at the start of the file." ),
                         false  // Default value
                       ) );

  driverMetadata.insert( "CSV",
                         MetaData(
                           "Comma Separated Value [CSV]",
                           QObject::tr( "Comma Separated Value [CSV]" ),
                           "*.csv",
                           "csv",
                           datasetOptions,
                           layerOptions
                         )
                       );

  // ESRI Shapefile
  datasetOptions.clear();
  layerOptions.clear();

  layerOptions.insert( "SHPT", new SetOption(
                         QObject::tr( "Override the type of shapefile created. "
                                      "Can be one of NULL for a simple .dbf file with no .shp file, POINT, "
                                      "ARC, POLYGON or MULTIPOINT for 2D, or POINTZ, ARCZ, POLYGONZ or "
                                      "MULTIPOINTZ for 3D. Shapefiles with measure values are not supported, "
                                      "nor are MULTIPATCH files." ),
                         QStringList()
                         << "NULL"
                         << "POINT"
                         << "ARC"
                         << "POLYGON"
                         << "MULTIPOINT"
                         << "POINTZ"
                         << "ARCZ"
                         << "POLYGONZ"
                         << "MULTIPOINTZ",
                         QString(), // Default value
                         true  // Allow None
                       ) );

  // there does not seem to be a reason to provide this option to the user again
  // as we set encoding for shapefiles based on "fileEncoding" parameter passed to the writer
#if 0
  layerOptions.insert( "ENCODING", new SetOption(
                         QObject::tr( "set the encoding value in the DBF file. "
                                      "The default value is LDID/87. It is not clear "
                                      "what other values may be appropriate." ),
                         QStringList()
                         << "LDID/87",
                         "LDID/87" // Default value
                       ) );
#endif

  layerOptions.insert( "RESIZE", new BoolOption(
                         QObject::tr( "Set to YES to resize fields to their optimal size." ),
                         false  // Default value
                       ) );

  driverMetadata.insert( "ESRI",
                         MetaData(
                           "ESRI Shapefile",
                           QObject::tr( "ESRI Shapefile" ),
                           "*.shp",
                           "shp",
                           datasetOptions,
                           layerOptions
                         )
                       );

  // DBF File
  datasetOptions.clear();
  layerOptions.clear();

  driverMetadata.insert( "DBF File",
                         MetaData(
                           "DBF File",
                           QObject::tr( "DBF File" ),
                           "*.dbf",
                           "dbf",
                           datasetOptions,
                           layerOptions
                         )
                       );

  // FMEObjects Gateway
  datasetOptions.clear();
  layerOptions.clear();

  driverMetadata.insert( "FMEObjects Gateway",
                         MetaData(
                           "FMEObjects Gateway",
                           QObject::tr( "FMEObjects Gateway" ),
                           "*.fdd",
                           "fdd",
                           datasetOptions,
                           layerOptions
                         )
                       );

  // GeoJSON
  datasetOptions.clear();
  layerOptions.clear();

  layerOptions.insert( "WRITE_BBOX", new BoolOption(
                         QObject::tr( "Set to YES to write a bbox property with the bounding box "
                                      "of the geometries at the feature and feature collection level." ),
                         false  // Default value
                       ) );

  layerOptions.insert( "COORDINATE_PRECISION", new IntOption(
                         QObject::tr( "Maximum number of figures after decimal separator to write in coordinates. "
                                      "Default to 15. Truncation will occur to remove trailing zeros." ),
                         15 // Default value
                       ) );

  driverMetadata.insert( "GeoJSON",
                         MetaData(
                           "GeoJSON",
                           QObject::tr( "GeoJSON" ),
                           "*.geojson",
                           "geojson",
                           datasetOptions,
                           layerOptions,
                           "UTF-8"
                         )
                       );

  // GeoRSS
  datasetOptions.clear();
  layerOptions.clear();

  datasetOptions.insert( "FORMAT", new SetOption(
                           QObject::tr( "whether the document must be in RSS 2.0 or Atom 1.0 format. "
                                        "Default value : RSS" ),
                           QStringList()
                           << "RSS"
                           << "ATOM",
                           "RSS" // Default value
                         ) );

  datasetOptions.insert( "GEOM_DIALECT", new SetOption(
                           QObject::tr( "The encoding of location information. Default value : SIMPLE. "
                                        "W3C_GEO only supports point geometries. "
                                        "SIMPLE or W3C_GEO only support geometries in geographic WGS84 coordinates." ),
                           QStringList()
                           << "SIMPLE"
                           << "GML"
                           << "W3C_GEO",
                           "SIMPLE" // Default value
                         ) );

  datasetOptions.insert( "USE_EXTENSIONS", new BoolOption(
                           QObject::tr( "If defined to YES, extension fields will be written. "
                                        "If the field name not found in the base schema matches "
                                        "the foo_bar pattern, foo will be considered as the namespace "
                                        "of the element, and a <foo:bar> element will be written. "
                                        "Otherwise, elements will be written in the <ogr:> namespace." ),
                           true  // Default value
                         ) );

  datasetOptions.insert( "WRITE_HEADER_AND_FOOTER", new BoolOption(
                           QObject::tr( "If defined to NO, only <entry> or <item> elements will be written. "
                                        "The user will have to provide the appropriate header and footer of the document." ),
                           true  // Default value
                         ) );

  datasetOptions.insert( "HEADER", new StringOption(
                           QObject::tr( "XML content that will be put between the <channel> element and the "
                                        "first <item> element for a RSS document, or between the xml tag and "
                                        "the first <entry> element for an Atom document. " ),
                           ""  // Default value
                         ) );

  datasetOptions.insert( "TITLE", new StringOption(
                           QObject::tr( "Value put inside the <title> element in the header. "
                                        "If not provided, a dummy value will be used as that element is compulsory." ),
                           ""  // Default value
                         ) );

  datasetOptions.insert( "DESCRIPTION", new StringOption(
                           QObject::tr( "Value put inside the <description> element in the header. "
                                        "If not provided, a dummy value will be used as that element is compulsory." ),
                           ""  // Default value
                         ) );

  datasetOptions.insert( "LINK", new StringOption(
                           QObject::tr( "Value put inside the <link> element in the header. "
                                        "If not provided, a dummy value will be used as that element is compulsory." ),
                           ""  // Default value
                         ) );

  datasetOptions.insert( "UPDATED", new StringOption(
                           QObject::tr( "Value put inside the <updated> element in the header. "
                                        "Should be formatted as a XML datetime. "
                                        "If not provided, a dummy value will be used as that element is compulsory." ),
                           ""  // Default value
                         ) );

  datasetOptions.insert( "AUTHOR_NAME", new StringOption(
                           QObject::tr( "Value put inside the <author><name> element in the header. "
                                        "If not provided, a dummy value will be used as that element is compulsory." ),
                           ""  // Default value
                         ) );

  datasetOptions.insert( "ID", new StringOption(
                           QObject::tr( "Value put inside the <id> element in the header. "
                                        "If not provided, a dummy value will be used as that element is compulsory." ),
                           ""  // Default value
                         ) );

  driverMetadata.insert( "GeoRSS",
                         MetaData(
                           "GeoRSS",
                           QObject::tr( "GeoRSS" ),
                           "*.xml",
                           "xml",
                           datasetOptions,
                           layerOptions,
                           "UTF-8"
                         )
                       );

  // Geography Markup Language [GML]
  datasetOptions.clear();
  layerOptions.clear();

  datasetOptions.insert( "XSISCHEMAURI", new StringOption(
                           QObject::tr( "If provided, this URI will be inserted as the schema location. "
                                        "Note that the schema file isn't actually accessed by OGR, so it "
                                        "is up to the user to ensure it will match the schema of the OGR "
                                        "produced GML data file." ),
                           ""  // Default value
                         ) );

  datasetOptions.insert( "XSISCHEMA", new SetOption(
                           QObject::tr( "This writes a GML application schema file to a corresponding "
                                        ".xsd file (with the same basename). If INTERNAL is used the "
                                        "schema is written within the GML file, but this is experimental "
                                        "and almost certainly not valid XML. "
                                        "OFF disables schema generation (and is implicit if XSISCHEMAURI is used)." ),
                           QStringList()
                           << "EXTERNAL"
                           << "INTERNAL"
                           << "OFF",
                           "EXTERNAL" // Default value
                         ) );

  datasetOptions.insert( "PREFIX", new StringOption(
                           QObject::tr( "This is the prefix for the application target namespace." ),
                           "ogr"  // Default value
                         ) );

  datasetOptions.insert( "STRIP_PREFIX", new BoolOption(
                           QObject::tr( "Can be set to TRUE to avoid writing the prefix of the "
                                        "application target namespace in the GML file." ),
                           false  // Default value
                         ) );

  datasetOptions.insert( "TARGET_NAMESPACE", new StringOption(
                           QObject::tr( "Defaults to 'http://ogr.maptools.org/'. "
                                        "This is the application target namespace." ),
                           "http://ogr.maptools.org/"  // Default value
                         ) );

  datasetOptions.insert( "FORMAT", new SetOption(
                           QObject::tr( "If not specified, GML2 will be used." ),
                           QStringList()
                           << "GML3"
                           << "GML3Deegree"
                           << "GML3.2",
                           "", // Default value
                           true // Allow None
                         ) );

  datasetOptions.insert( "GML3_LONGSRS", new BoolOption(
                           QObject::tr( "only valid when FORMAT=GML3/GML3Degree/GML3.2) Default to YES. "
                                        "If YES, SRS with EPSG authority will be written with the "
                                        "'urn:ogc:def:crs:EPSG::' prefix. In the case, if the SRS is a "
                                        "geographic SRS without explicit AXIS order, but that the same "
                                        "SRS authority code imported with ImportFromEPSGA() should be "
                                        "treated as lat/long, then the function will take care of coordinate "
                                        "order swapping. If set to NO, SRS with EPSG authority will be "
                                        "written with the 'EPSG:' prefix, even if they are in lat/long order." ),
                           true  // Default value
                         ) );

  datasetOptions.insert( "WRITE_FEATURE_BOUNDED_BY", new BoolOption(
                           QObject::tr( "only valid when FORMAT=GML3/GML3Degree/GML3.2) Default to YES. "
                                        "If set to NO, the <gml:boundedBy> element will not be written for "
                                        "each feature." ),
                           true  // Default value
                         ) );

  datasetOptions.insert( "SPACE_INDENTATION", new BoolOption(
                           QObject::tr( "Default to YES. If YES, the output will be indented with spaces "
                                        "for more readability, but at the expense of file size." ),
                           true  // Default value
                         ) );


  driverMetadata.insert( "GML",
                         MetaData(
                           "Geography Markup Language [GML]",
                           QObject::tr( "Geography Markup Language [GML]" ),
                           "*.gml",
                           "gml",
                           datasetOptions,
                           layerOptions,
                           "UTF-8"
                         )
                       );

  // GeoPackage
  datasetOptions.clear();
  layerOptions.clear();

  layerOptions.insert( "IDENTIFIER", new StringOption(
                         QObject::tr( "Human-readable identifier (e.g. short name) for the layer content" ),
                         ""  // Default value
                       ) );

  layerOptions.insert( "DESCRIPTION", new StringOption(
                         QObject::tr( "Human-readable description for the layer content" ),
                         ""  // Default value
                       ) );

  layerOptions.insert( "FID", new StringOption(
                         QObject::tr( "Name for the feature identifier column" ),
                         "fid"  // Default value
                       ) );

  layerOptions.insert( "GEOMETRY_NAME", new StringOption(
                         QObject::tr( "Name for the geometry column" ),
                         "geometry"  // Default value
                       ) );

#if defined(GDAL_COMPUTE_VERSION) && GDAL_VERSION_NUM >= GDAL_COMPUTE_VERSION(2,0,0)
  layerOptions.insert( "SPATIAL_INDEX", new BoolOption(
                         QObject::tr( "If a spatial index must be created." ),
                         true  // Default value
                       ) );
#endif

  driverMetadata.insert( "GPKG",
                         MetaData(
                           "GeoPackage",
                           QObject::tr( "GeoPackage" ),
                           "*.gpkg",
                           "gpkg",
                           datasetOptions,
                           layerOptions,
                           "UTF-8"
                         )
                       );

  // Generic Mapping Tools [GMT]
  datasetOptions.clear();
  layerOptions.clear();

  driverMetadata.insert( "GMT",
                         MetaData(
                           "Generic Mapping Tools [GMT]",
                           QObject::tr( "Generic Mapping Tools [GMT]" ),
                           "*.gmt",
                           "gmt",
                           datasetOptions,
                           layerOptions
                         )
                       );

  // GPS eXchange Format [GPX]
  datasetOptions.clear();
  layerOptions.clear();

  layerOptions.insert( "FORCE_GPX_TRACK", new BoolOption(
                         QObject::tr( "By default when writing a layer whose features are of "
                                      "type wkbLineString, the GPX driver chooses to write "
                                      "them as routes. If FORCE_GPX_TRACK=YES is specified, "
                                      "they will be written as tracks." ),
                         false  // Default value
                       ) );

  layerOptions.insert( "FORCE_GPX_ROUTE", new BoolOption(
                         QObject::tr( "By default when writing a layer whose features are of "
                                      "type wkbMultiLineString, the GPX driver chooses to write "
                                      "them as tracks. If FORCE_GPX_ROUTE=YES is specified, "
                                      "they will be written as routes, provided that the multilines "
                                      "are composed of only one single line." ),
                         false  // Default value
                       ) );

  datasetOptions.insert( "GPX_USE_EXTENSIONS", new BoolOption(
                           QObject::tr( "If GPX_USE_EXTENSIONS=YES is specified, "
                                        "extra fields will be written inside the <extensions> tag." ),
                           true  // Default value
                         ) );

  datasetOptions.insert( "GPX_EXTENSIONS_NS", new StringOption(
                           QObject::tr( "Only used if GPX_USE_EXTENSIONS=YES and GPX_EXTENSIONS_NS_URL "
                                        "is set. The namespace value used for extension tags. By default, 'ogr'." ),
                           "ogr"  // Default value
                         ) );

  datasetOptions.insert( "GPX_EXTENSIONS_NS_URL", new StringOption(
                           QObject::tr( "Only used if GPX_USE_EXTENSIONS=YES and GPX_EXTENSIONS_NS "
                                        "is set. The namespace URI. By default, 'http://osgeo.org/gdal'." ),
                           "http://osgeo.org/gdal"  // Default value
                         ) );

  datasetOptions.insert( "LINEFORMAT", new SetOption(
                           QObject::tr( "By default files are created with the line termination "
                                        "conventions of the local platform (CR/LF on win32 or LF "
                                        "on all other systems). This may be overridden through use "
                                        "of the LINEFORMAT layer creation option which may have a value "
                                        "of CRLF (DOS format) or LF (Unix format)." ),
                           QStringList()
                           << "CRLF"
                           << "LF",
                           "", // Default value
                           true // Allow None
                         ) );

  driverMetadata.insert( "GPX",
                         MetaData(
                           "GPS eXchange Format [GPX]",
                           QObject::tr( "GPS eXchange Format [GPX]" ),
                           "*.gpx",
                           "gpx",
                           datasetOptions,
                           layerOptions,
                           "UTF-8"
                         )
                       );

  // INTERLIS 1
  datasetOptions.clear();
  layerOptions.clear();

  driverMetadata.insert( "Interlis 1",
                         MetaData(
                           "INTERLIS 1",
                           QObject::tr( "INTERLIS 1" ),
                           "*.itf *.xml *.ili",
                           "ili",
                           datasetOptions,
                           layerOptions
                         )
                       );

  // INTERLIS 2
  datasetOptions.clear();
  layerOptions.clear();

  driverMetadata.insert( "Interlis 2",
                         MetaData(
                           "INTERLIS 2",
                           QObject::tr( "INTERLIS 2" ),
                           "*.itf *.xml *.ili",
                           "ili",
                           datasetOptions,
                           layerOptions
                         )
                       );

  // Keyhole Markup Language [KML]
  datasetOptions.clear();
  layerOptions.clear();

  datasetOptions.insert( "NameField", new StringOption(
                           QObject::tr( "Allows you to specify the field to use for the KML <name> element. " ),
                           "Name"  // Default value
                         ) );

  datasetOptions.insert( "DescriptionField", new StringOption(
                           QObject::tr( "Allows you to specify the field to use for the KML <description> element." ),
                           "Description"  // Default value
                         ) );

  datasetOptions.insert( "AltitudeMode", new SetOption(
                           QObject::tr( "Allows you to specify the AltitudeMode to use for KML geometries. "
                                        "This will only affect 3D geometries and must be one of the valid KML options." ),
                           QStringList()
                           << "clampToGround"
                           << "relativeToGround"
                           << "absolute",
                           "clampToGround" // Default value
                         ) );

  driverMetadata.insert( "KML",
                         MetaData(
                           "Keyhole Markup Language [KML]",
                           QObject::tr( "Keyhole Markup Language [KML]" ),
                           "*.kml",
                           "kml",
                           datasetOptions,
                           layerOptions,
                           "UTF-8"
                         )
                       );

  // Mapinfo
  datasetOptions.clear();
  layerOptions.clear();

  layerOptions.insert( "SPATIAL_INDEX_MODE", new SetOption(
                         QObject::tr( "Use this to turn on 'quick spatial index mode'. "
                                      "In this mode writing files can be about 5 times faster, "
                                      "but spatial queries can be up to 30 times slower." ),
                         QStringList()
                         << "QUICK",
                         "", // Default value
                         true // Allow None
                       ) );

  driverMetadata.insert( "MapInfo File",
                         MetaData(
                           "Mapinfo",
                           QObject::tr( "Mapinfo TAB" ),
                           "*.tab",
                           "tab",
                           datasetOptions,
                           layerOptions
                         )
                       );

  // QGIS internal alias for MIF files
  driverMetadata.insert( "MapInfo MIF",
                         MetaData(
                           "Mapinfo",
                           QObject::tr( "Mapinfo MIF" ),
                           "*.mif",
                           "mif",
                           datasetOptions,
                           layerOptions
                         )
                       );

  // Microstation DGN
  datasetOptions.clear();
  layerOptions.clear();

  datasetOptions.insert( "3D", new BoolOption(
                           QObject::tr( "Determine whether 2D (seed_2d.dgn) or 3D (seed_3d.dgn) "
                                        "seed file should be used. This option is ignored if the SEED option is provided." ),
                           false  // Default value
                         ) );

  datasetOptions.insert( "SEED", new StringOption(
                           QObject::tr( "Override the seed file to use." ),
                           ""  // Default value
                         ) );

  datasetOptions.insert( "COPY_WHOLE_SEED_FILE", new BoolOption(
                           QObject::tr( "Indicate whether the whole seed file should be copied. "
                                        "If not, only the first three elements will be copied." ),
                           false  // Default value
                         ) );

  datasetOptions.insert( "COPY_SEED_FILE_COLOR_TABLEE", new BoolOption(
                           QObject::tr( "Indicates whether the color table should be copied from the seed file." ),
                           false  // Default value
                         ) );

  datasetOptions.insert( "MASTER_UNIT_NAME", new StringOption(
                           QObject::tr( "Override the master unit name from the seed file with "
                                        "the provided one or two character unit name." ),
                           ""  // Default value
                         ) );

  datasetOptions.insert( "SUB_UNIT_NAME", new StringOption(
                           QObject::tr( "Override the sub unit name from the seed file with the provided "
                                        "one or two character unit name." ),
                           ""  // Default value
                         ) );

  datasetOptions.insert( "SUB_UNITS_PER_MASTER_UNIT", new IntOption(
                           QObject::tr( "Override the number of subunits per master unit. "
                                        "By default the seed file value is used." ),
                           0 // Default value
                         ) );

  datasetOptions.insert( "UOR_PER_SUB_UNIT", new IntOption(
                           QObject::tr( "Override the number of UORs (Units of Resolution) "
                                        "per sub unit. By default the seed file value is used." ),
                           0 // Default value
                         ) );

  datasetOptions.insert( "ORIGIN", new StringOption(
                           QObject::tr( "ORIGIN=x,y,z: Override the origin of the design plane. "
                                        "By default the origin from the seed file is used." ),
                           ""  // Default value
                         ) );

  driverMetadata.insert( "DGN",
                         MetaData(
                           "Microstation DGN",
                           QObject::tr( "Microstation DGN" ),
                           "*.dgn",
                           "dgn",
                           datasetOptions,
                           layerOptions
                         )
                       );

  // S-57 Base file
  datasetOptions.clear();
  layerOptions.clear();

  datasetOptions.insert( "UPDATES", new SetOption(
                           QObject::tr( "Should update files be incorporated into the base data on the fly. " ),
                           QStringList()
                           << "APPLY"
                           << "IGNORE",
                           "APPLY" // Default value
                         ) );

  datasetOptions.insert( "SPLIT_MULTIPOINT", new BoolOption(
                           QObject::tr( "Should multipoint soundings be split into many single point sounding features. "
                                        "Multipoint geometries are not well handled by many formats, "
                                        "so it can be convenient to split single sounding features with many points "
                                        "into many single point features." ),
                           false  // Default value
                         ) );

  datasetOptions.insert( "ADD_SOUNDG_DEPTH", new BoolOption(
                           QObject::tr( "Should a DEPTH attribute be added on SOUNDG features and assign the depth "
                                        "of the sounding. This should only be enabled with SPLIT_MULTIPOINT is "
                                        "also enabled." ),
                           false  // Default value
                         ) );

  datasetOptions.insert( "RETURN_PRIMITIVES", new BoolOption(
                           QObject::tr( "Should all the low level geometry primitives be returned as special "
                                        "IsolatedNode, ConnectedNode, Edge and Face layers." ),
                           true  // Default value
                         ) );

  datasetOptions.insert( "PRESERVE_EMPTY_NUMBERS", new BoolOption(
                           QObject::tr( "If enabled, numeric attributes assigned an empty string as a value will "
                                        "be preserved as a special numeric value. This option should not generally "
                                        "be needed, but may be useful when translated S-57 to S-57 losslessly." ),
                           false  // Default value
                         ) );

  datasetOptions.insert( "LNAM_REFS", new BoolOption(
                           QObject::tr( "Should LNAM and LNAM_REFS fields be attached to features capturing "
                                        "the feature to feature relationships in the FFPT group of the S-57 file." ),
                           true  // Default value
                         ) );

  datasetOptions.insert( "RETURN_LINKAGES", new BoolOption(
                           QObject::tr( "Should additional attributes relating features to their underlying "
                                        "geometric primitives be attached. These are the values of the FSPT group, "
                                        "and are primarily needed when doing S-57 to S-57 translations." ),
                           true  // Default value
                         ) );

  datasetOptions.insert( "RECODE_BY_DSSI", new BoolOption(
                           QObject::tr( "Should attribute values be recoded to UTF-8 from the character encoding "
                                        "specified in the S57 DSSI record." ),
                           false  // Default value
                         ) );

  // set OGR_S57_OPTIONS = "RETURN_PRIMITIVES=ON,RETURN_LINKAGES=ON,LNAM_REFS=ON"

  driverMetadata.insert( "S57",
                         MetaData(
                           "S-57 Base file",
                           QObject::tr( "S-57 Base file" ),
                           "*.000",
                           "000",
                           datasetOptions,
                           layerOptions
                         )
                       );

  // Spatial Data Transfer Standard [SDTS]
  datasetOptions.clear();
  layerOptions.clear();

  driverMetadata.insert( "SDTS",
                         MetaData(
                           "Spatial Data Transfer Standard [SDTS]",
                           QObject::tr( "Spatial Data Transfer Standard [SDTS]" ),
                           "*catd.ddf",
                           "ddf",
                           datasetOptions,
                           layerOptions
                         )
                       );

  // SQLite
  datasetOptions.clear();
  layerOptions.clear();

  datasetOptions.insert( "METADATA", new BoolOption(
                           QObject::tr( "Can be used to avoid creating the geometry_columns and spatial_ref_sys "
                                        "tables in a new database. By default these metadata tables are created "
                                        "when a new database is created." ),
                           true  // Default value
                         ) );

  // Will handle the spatialite alias
  datasetOptions.insert( "SPATIALITE", new HiddenOption(
                           "NO"
                         ) );


  datasetOptions.insert( "INIT_WITH_EPSG", new HiddenOption(
                           "NO"
                         ) );

  layerOptions.insert( "FORMAT", new SetOption(
                         QObject::tr( "Controls the format used for the geometry column. Defaults to WKB."
                                      "This is generally more space and processing efficient, but harder "
                                      "to inspect or use in simple applications than WKT (Well Known Text)." ),
                         QStringList()
                         << "WKB"
                         << "WKT",
                         "WKB" // Default value
                       ) );

  layerOptions.insert( "LAUNDER", new BoolOption(
                         QObject::tr( "Controls whether layer and field names will be laundered for easier use "
                                      "in SQLite. Laundered names will be converted to lower case and some special "
                                      "characters(' - #) will be changed to underscores." ),
                         true  // Default value
                       ) );

  layerOptions.insert( "SPATIAL_INDEX", new HiddenOption(
                         "NO"
                       ) );

  layerOptions.insert( "COMPRESS_GEOM", new HiddenOption(
                         "NO"
                       ) );

  layerOptions.insert( "SRID", new HiddenOption(
                         ""
                       ) );

  layerOptions.insert( "COMPRESS_COLUMNS", new StringOption(
                         QObject::tr( "column_name1[,column_name2, ...] A list of (String) columns that "
                                      "must be compressed with ZLib DEFLATE algorithm. This might be beneficial "
                                      "for databases that have big string blobs. However, use with care, since "
                                      "the value of such columns will be seen as compressed binary content with "
                                      "other SQLite utilities (or previous OGR versions). With OGR, when inserting, "
                                      "modifying or queryings compressed columns, compression/decompression is "
                                      "done transparently. However, such columns cannot be (easily) queried with "
                                      "an attribute filter or WHERE clause. Note: in table definition, such columns "
                                      "have the 'VARCHAR_deflate' declaration type." ),
                         ""  // Default value
                       ) );

  driverMetadata.insert( "SQLite",
                         MetaData(
                           "SQLite",
                           QObject::tr( "SQLite" ),
                           "*.sqlite",
                           "sqlite",
                           datasetOptions,
                           layerOptions,
                           "UTF-8"
                         )
                       );

  // SpatiaLite
  datasetOptions.clear();
  layerOptions.clear();

  datasetOptions.insert( "METADATA", new BoolOption(
                           QObject::tr( "Can be used to avoid creating the geometry_columns and spatial_ref_sys "
                                        "tables in a new database. By default these metadata tables are created "
                                        "when a new database is created." ),
                           true  // Default value
                         ) );

  datasetOptions.insert( "SPATIALITE", new HiddenOption(
                           "YES"
                         ) );

  datasetOptions.insert( "INIT_WITH_EPSG", new BoolOption(
                           QObject::tr( "Insert the content of the EPSG CSV files into the spatial_ref_sys table. "
                                        "Set to NO for regular SQLite databases." ),
                           true  // Default value
                         ) );

  layerOptions.insert( "FORMAT", new HiddenOption(
                         "SPATIALITE"
                       ) );

  layerOptions.insert( "LAUNDER", new BoolOption(
                         QObject::tr( "Controls whether layer and field names will be laundered for easier use "
                                      "in SQLite. Laundered names will be converted to lower case and some special "
                                      "characters(' - #) will be changed to underscores." ),
                         true  // Default value
                       ) );

  layerOptions.insert( "SPATIAL_INDEX", new BoolOption(
                         QObject::tr( "If the database is of the SpatiaLite flavour, and if OGR is linked "
                                      "against libspatialite, this option can be used to control if a spatial "
                                      "index must be created." ),
                         true  // Default value
                       ) );

  layerOptions.insert( "COMPRESS_GEOM", new BoolOption(
                         QObject::tr( "If the format of the geometry BLOB is of the SpatiaLite flavour, "
                                      "this option can be used to control if the compressed format for "
                                      "geometries (LINESTRINGs, POLYGONs) must be used" ),
                         false  // Default value
                       ) );

  layerOptions.insert( "SRID", new StringOption(
                         QObject::tr( "Used to force the SRID number of the SRS associated with the layer. "
                                      "When this option isn't specified and that a SRS is associated with the "
                                      "layer, a search is made in the spatial_ref_sys to find a match for the "
                                      "SRS, and, if there is no match, a new entry is inserted for the SRS in "
                                      "the spatial_ref_sys table. When the SRID option is specified, this "
                                      "search (and the eventual insertion of a new entry) will not be done: "
                                      "the specified SRID is used as such." ),
                         ""  // Default value
                       ) );

  layerOptions.insert( "COMPRESS_COLUMNS", new StringOption(
                         QObject::tr( "column_name1[,column_name2, ...] A list of (String) columns that "
                                      "must be compressed with ZLib DEFLATE algorithm. This might be beneficial "
                                      "for databases that have big string blobs. However, use with care, since "
                                      "the value of such columns will be seen as compressed binary content with "
                                      "other SQLite utilities (or previous OGR versions). With OGR, when inserting, "
                                      "modifying or queryings compressed columns, compression/decompression is "
                                      "done transparently. However, such columns cannot be (easily) queried with "
                                      "an attribute filter or WHERE clause. Note: in table definition, such columns "
                                      "have the 'VARCHAR_deflate' declaration type." ),
                         ""  // Default value
                       ) );

  driverMetadata.insert( "SpatiaLite",
                         MetaData(
                           "SpatiaLite",
                           QObject::tr( "SpatiaLite" ),
                           "*.sqlite",
                           "sqlite",
                           datasetOptions,
                           layerOptions,
                           "UTF-8"
                         )
                       );
  // AutoCAD DXF
  datasetOptions.clear();
  layerOptions.clear();

#if 0
  datasetOptions.insert( "HEADER", new StringOption(
                           QObject::tr( "Override the header file used - in place of header.dxf." ),
                           ""  // Default value
                         ) );

  datasetOptions.insert( "TRAILER", new StringOption(
                           QObject::tr( "Override the trailer file used - in place of trailer.dxf." ),
                           ""  // Default value
                         ) );
#endif

  driverMetadata.insert( "DXF",
                         MetaData(
                           "AutoCAD DXF",
                           QObject::tr( "AutoCAD DXF" ),
                           "*.dxf",
                           "dxf",
                           datasetOptions,
                           layerOptions
                         )
                       );

  // Geoconcept
  datasetOptions.clear();
  layerOptions.clear();

  datasetOptions.insert( "EXTENSION", new SetOption(
                           QObject::tr( "Indicates the GeoConcept export file extension. "
                                        "TXT was used by earlier releases of GeoConcept. GXT is currently used." ),
                           QStringList()
                           << "GXT"
                           << "TXT",
                           "GXT" // Default value
                         ) );

#if 0
  datasetOptions.insert( "CONFIG", new StringOption(
                           QObject::tr( "path to the GCT : the GCT file describe the GeoConcept types definitions: "
                                        "In this file, every line must start with //# followed by a keyword. "
                                        "Lines starting with // are comments." ),
                           ""  // Default value
                         ) );
#endif

  driverMetadata.insert( "Geoconcept",
                         MetaData(
                           "Geoconcept",
                           QObject::tr( "Geoconcept" ),
                           "*.gxt *.txt",
                           "gxt",
                           datasetOptions,
                           layerOptions
                         )
                       );

  // ESRI FileGDB
  datasetOptions.clear();
  layerOptions.clear();

  layerOptions.insert( "FEATURE_DATASET", new StringOption(
                         QObject::tr( "When this option is set, the new layer will be created inside the named "
                                      "FeatureDataset folder. If the folder does not already exist, it will be created." ),
                         ""  // Default value
                       ) );

  layerOptions.insert( "GEOMETRY_NAME", new StringOption(
                         QObject::tr( "Set name of geometry column in new layer. Defaults to 'SHAPE'." ),
                         "SHAPE"  // Default value
                       ) );

  layerOptions.insert( "OID_NAME", new StringOption(
                         QObject::tr( "Name of the OID column to create. Defaults to 'OBJECTID'." ),
                         "OBJECTID"  // Default value
                       ) );

  driverMetadata.insert( "FileGDB",
                         MetaData(
                           "ESRI FileGDB",
                           QObject::tr( "ESRI FileGDB" ),
                           "*.gdb",
                           "gdb",
                           datasetOptions,
                           layerOptions,
                           "UTF-8"
                         )
                       );

  // XLSX
  datasetOptions.clear();
  layerOptions.clear();

  layerOptions.insert( "OGR_XLSX_FIELD_TYPES", new SetOption(
                         QObject::tr( "By default, the driver will try to detect the data type of fields. If set "
                                      "to STRING, all fields will be of String type." ),
                         QStringList()
                         << "AUTO"
                         << "STRING",
                         "AUTO", // Default value
                         false // Allow None
                       ) );

  driverMetadata.insert( "XLSX",
                         MetaData(
                           "MS Office Open XML spreadsheet",
                           QObject::tr( "MS Office Open XML spreadsheet" ),
                           "*.xlsx",
                           "xlsx",
                           datasetOptions,
                           layerOptions,
                           "UTF-8"
                         )
                       );

  // ODS
  datasetOptions.clear();
  layerOptions.clear();

  layerOptions.insert( "OGR_ODS_FIELD_TYPES", new SetOption(
                         QObject::tr( "By default, the driver will try to detect the data type of fields. If set "
                                      "to STRING, all fields will be of String type." ),
                         QStringList()
                         << "AUTO"
                         << "STRING",
                         "AUTO", // Default value
                         false // Allow None
                       ) );

  driverMetadata.insert( "ODS",
                         MetaData(
                           "Open Document Spreadsheet",
                           QObject::tr( "Open Document Spreadsheet" ),
                           "*.ods",
                           "ods",
                           datasetOptions,
                           layerOptions,
                           "UTF-8"
                         )
                       );

  return driverMetadata;
}

bool QgsVectorFileWriter::driverMetadata( const QString& driverName, QgsVectorFileWriter::MetaData& driverMetadata )
{
  static const QMap<QString, MetaData> sDriverMetadata = initMetaData();

  QMap<QString, MetaData>::ConstIterator it = sDriverMetadata.constBegin();

  for ( ; it != sDriverMetadata.constEnd(); ++it )
  {
    if ( it.key().startsWith( driverName ) || it.value().longName.startsWith( driverName ) )
    {
      driverMetadata = it.value();
      return true;
    }
  }

  return false;
}

QStringList QgsVectorFileWriter::defaultDatasetOptions( const QString& driverName )
{
  MetaData metadata;
  bool ok = driverMetadata( driverName, metadata );
  if ( !ok )
    return QStringList();
  return concatenateOptions( metadata.driverOptions );
}

QStringList QgsVectorFileWriter::defaultLayerOptions( const QString& driverName )
{
  MetaData metadata;
  bool ok = driverMetadata( driverName, metadata );
  if ( !ok )
    return QStringList();
  return concatenateOptions( metadata.layerOptions );
}

OGRwkbGeometryType QgsVectorFileWriter::ogrTypeFromWkbType( QgsWkbTypes::Type type )
{
  type = QgsWkbTypes::dropM( type );

  OGRwkbGeometryType ogrType = static_cast<OGRwkbGeometryType>( type );

  if ( type >= QgsWkbTypes::PointZ && type <= QgsWkbTypes::GeometryCollectionZ )
  {
    ogrType = static_cast<OGRwkbGeometryType>( QgsWkbTypes::to25D( type ) );
  }
  return ogrType;
}

QgsVectorFileWriter::WriterError QgsVectorFileWriter::hasError()
{
  return mError;
}

QString QgsVectorFileWriter::errorMessage()
{
  return mErrorMessage;
}

bool QgsVectorFileWriter::addFeature( QgsFeature& feature, QgsFeatureRenderer* renderer, QgsUnitTypes::DistanceUnit outputUnit )
{
  // create the feature
  OGRFeatureH poFeature = createFeature( feature );
  if ( !poFeature )
    return false;

  //add OGR feature style type
  if ( mSymbologyExport != NoSymbology && renderer )
  {
    mRenderContext.expressionContext().setFeature( feature );
    //SymbolLayerSymbology: concatenate ogr styles of all symbollayers
    QgsSymbolList symbols = renderer->symbolsForFeature( feature, mRenderContext );
    QString styleString;
    QString currentStyle;

    QgsSymbolList::const_iterator symbolIt = symbols.constBegin();
    for ( ; symbolIt != symbols.constEnd(); ++symbolIt )
    {
      int nSymbolLayers = ( *symbolIt )->symbolLayerCount();
      for ( int i = 0; i < nSymbolLayers; ++i )
      {
#if 0
        QMap< QgsSymbolLayer*, QString >::const_iterator it = mSymbolLayerTable.find(( *symbolIt )->symbolLayer( i ) );
        if ( it == mSymbolLayerTable.constEnd() )
        {
          continue;
        }
#endif
        double mmsf = mmScaleFactor( mSymbologyScaleDenominator, ( *symbolIt )->outputUnit(), outputUnit );
        double musf = mapUnitScaleFactor( mSymbologyScaleDenominator, ( *symbolIt )->outputUnit(), outputUnit );

        currentStyle = ( *symbolIt )->symbolLayer( i )->ogrFeatureStyle( mmsf, musf );//"@" + it.value();

        if ( mSymbologyExport == FeatureSymbology )
        {
          if ( symbolIt != symbols.constBegin() || i != 0 )
          {
            styleString.append( ';' );
          }
          styleString.append( currentStyle );
        }
        else if ( mSymbologyExport == SymbolLayerSymbology )
        {
          OGR_F_SetStyleString( poFeature, currentStyle.toLocal8Bit().constData() );
          if ( !writeFeature( mLayer, poFeature ) )
          {
            return false;
          }
        }
      }
    }
    OGR_F_SetStyleString( poFeature, styleString.toLocal8Bit().constData() );
  }

  if ( mSymbologyExport == NoSymbology || mSymbologyExport == FeatureSymbology )
  {
    if ( !writeFeature( mLayer, poFeature ) )
    {
      return false;
    }
  }

  OGR_F_Destroy( poFeature );
  return true;
}

OGRFeatureH QgsVectorFileWriter::createFeature( const QgsFeature& feature )
{
  QgsLocaleNumC l; // Make sure the decimal delimiter is a dot
  Q_UNUSED( l );

  OGRFeatureH poFeature = OGR_F_Create( OGR_L_GetLayerDefn( mLayer ) );

  qint64 fid = FID_TO_NUMBER( feature.id() );
  if ( fid > std::numeric_limits<int>::max() )
  {
    QgsDebugMsg( QString( "feature id %1 too large." ).arg( fid ) );
    OGRErr err = OGR_F_SetFID( poFeature, static_cast<long>( fid ) );
    if ( err != OGRERR_NONE )
    {
      QgsDebugMsg( QString( "Failed to set feature id to %1: %2 (OGR error: %3)" )
                   .arg( feature.id() )
                   .arg( err ).arg( CPLGetLastErrorMsg() )
                 );
    }
  }

  // attribute handling
  for ( QMap<int, int>::const_iterator it = mAttrIdxToOgrIdx.constBegin(); it != mAttrIdxToOgrIdx.constEnd(); ++it )
  {
    int fldIdx = it.key();
    int ogrField = it.value();

    QVariant attrValue = feature.attribute( fldIdx );

    if ( !attrValue.isValid() || attrValue.isNull() )
      continue;

    if ( mFieldValueConverter )
    {
      attrValue = mFieldValueConverter->convert( fldIdx, attrValue );
    }

    switch ( attrValue.type() )
    {
#if defined(GDAL_VERSION_NUM) && GDAL_VERSION_NUM >= 2000000
      case QVariant::Int:
      case QVariant::UInt:
        OGR_F_SetFieldInteger( poFeature, ogrField, attrValue.toInt() );
        break;
      case QVariant::LongLong:
      case QVariant::ULongLong:
        OGR_F_SetFieldInteger64( poFeature, ogrField, attrValue.toLongLong() );
        break;
      case QVariant::String:
        OGR_F_SetFieldString( poFeature, ogrField, mCodec->fromUnicode( attrValue.toString() ).constData() );
        break;
#else
      case QVariant::Int:
        OGR_F_SetFieldInteger( poFeature, ogrField, attrValue.toInt() );
        break;
      case QVariant::String:
      case QVariant::LongLong:
      case QVariant::UInt:
      case QVariant::ULongLong:
        OGR_F_SetFieldString( poFeature, ogrField, mCodec->fromUnicode( attrValue.toString() ).constData() );
        break;
#endif
      case QVariant::Double:
        OGR_F_SetFieldDouble( poFeature, ogrField, attrValue.toDouble() );
        break;
      case QVariant::Date:
        OGR_F_SetFieldDateTime( poFeature, ogrField,
                                attrValue.toDate().year(),
                                attrValue.toDate().month(),
                                attrValue.toDate().day(),
                                0, 0, 0, 0 );
        break;
      case QVariant::DateTime:
        if ( mOgrDriverName == "ESRI Shapefile" )
        {
          OGR_F_SetFieldString( poFeature, ogrField, mCodec->fromUnicode( attrValue.toDateTime().toString( "yyyy/MM/dd hh:mm:ss.zzz" ) ).constData() );
        }
        else
        {
          OGR_F_SetFieldDateTime( poFeature, ogrField,
                                  attrValue.toDateTime().date().year(),
                                  attrValue.toDateTime().date().month(),
                                  attrValue.toDateTime().date().day(),
                                  attrValue.toDateTime().time().hour(),
                                  attrValue.toDateTime().time().minute(),
                                  attrValue.toDateTime().time().second(),
                                  0 );
        }
        break;
      case QVariant::Time:
        if ( mOgrDriverName == "ESRI Shapefile" )
        {
          OGR_F_SetFieldString( poFeature, ogrField, mCodec->fromUnicode( attrValue.toString() ).constData() );
        }
        else
        {
          OGR_F_SetFieldDateTime( poFeature, ogrField,
                                  0, 0, 0,
                                  attrValue.toTime().hour(),
                                  attrValue.toTime().minute(),
                                  attrValue.toTime().second(),
                                  0 );
        }
        break;
      case QVariant::Invalid:
        break;
      default:
        mErrorMessage = QObject::tr( "Invalid variant type for field %1[%2]: received %3 with type %4" )
                        .arg( mFields.at( fldIdx ).name() )
                        .arg( ogrField )
                        .arg( attrValue.typeName(),
                              attrValue.toString() );
        QgsMessageLog::logMessage( mErrorMessage, QObject::tr( "OGR" ) );
        mError = ErrFeatureWriteFailed;
        return nullptr;
    }
  }

  if ( mWkbType != QgsWkbTypes::NoGeometry )
  {
    if ( feature.hasGeometry() )
    {
      // build geometry from WKB
      QgsGeometry geom = feature.geometry();

      // turn single geometry to multi geometry if needed
      if ( QgsWkbTypes::flatType( geom.geometry()->wkbType() ) != QgsWkbTypes::flatType( mWkbType ) &&
           QgsWkbTypes::flatType( geom.geometry()->wkbType() ) == QgsWkbTypes::flatType( QgsWkbTypes::singleType( mWkbType ) ) )
      {
        geom.convertToMultiType();
      }

      if ( geom.geometry()->wkbType() != mWkbType )
      {
        OGRGeometryH mGeom2 = nullptr;

        // If requested WKB type is 25D and geometry WKB type is 3D,
        // we must force the use of 25D.
        if ( mWkbType >= QgsWkbTypes::Point25D && mWkbType <= QgsWkbTypes::MultiPolygon25D )
        {
          //ND: I suspect there's a bug here, in that this is NOT converting the geometry's WKB type,
          //so the exported WKB has a different type to what the OGRGeometry is expecting.
          //possibly this is handled already in OGR, but it should be fixed regardless by actually converting
          //geom to the correct WKB type
          QgsWkbTypes::Type wkbType = geom.geometry()->wkbType();
          if ( wkbType >= QgsWkbTypes::PointZ && wkbType <= QgsWkbTypes::MultiPolygonZ )
          {
            QgsWkbTypes::Type wkbType25d = static_cast<QgsWkbTypes::Type>( geom.geometry()->wkbType() - QgsWkbTypes::PointZ + QgsWkbTypes::Point25D );
            mGeom2 = createEmptyGeometry( wkbType25d );
          }
        }

        if ( !mGeom2 )
        {
          // there's a problem when layer type is set as wkbtype Polygon
          // although there are also features of type MultiPolygon
          // (at least in OGR provider)
          // If the feature's wkbtype is different from the layer's wkbtype,
          // try to export it too.
          //
          // Btw. OGRGeometry must be exactly of the type of the geometry which it will receive
          // i.e. Polygons can't be imported to OGRMultiPolygon
          mGeom2 = createEmptyGeometry( geom.geometry()->wkbType() );
        }

        if ( !mGeom2 )
        {
          mErrorMessage = QObject::tr( "Feature geometry not imported (OGR error: %1)" )
                          .arg( QString::fromUtf8( CPLGetLastErrorMsg() ) );
          mError = ErrFeatureWriteFailed;
          QgsMessageLog::logMessage( mErrorMessage, QObject::tr( "OGR" ) );
          OGR_F_Destroy( poFeature );
          return nullptr;
        }

        OGRErr err = OGR_G_ImportFromWkb( mGeom2, const_cast<unsigned char *>( geom.asWkb() ), static_cast< int >( geom.wkbSize() ) );
        if ( err != OGRERR_NONE )
        {
          mErrorMessage = QObject::tr( "Feature geometry not imported (OGR error: %1)" )
                          .arg( QString::fromUtf8( CPLGetLastErrorMsg() ) );
          mError = ErrFeatureWriteFailed;
          QgsMessageLog::logMessage( mErrorMessage, QObject::tr( "OGR" ) );
          OGR_F_Destroy( poFeature );
          return nullptr;
        }

        // pass ownership to geometry
        OGR_F_SetGeometryDirectly( poFeature, mGeom2 );
      }
      else // wkb type matches
      {
        OGRErr err = OGR_G_ImportFromWkb( mGeom, const_cast<unsigned char *>( geom.asWkb() ), static_cast< int >( geom.wkbSize() ) );
        if ( err != OGRERR_NONE )
        {
          mErrorMessage = QObject::tr( "Feature geometry not imported (OGR error: %1)" )
                          .arg( QString::fromUtf8( CPLGetLastErrorMsg() ) );
          mError = ErrFeatureWriteFailed;
          QgsMessageLog::logMessage( mErrorMessage, QObject::tr( "OGR" ) );
          OGR_F_Destroy( poFeature );
          return nullptr;
        }

        // set geometry (ownership is not passed to OGR)
        OGR_F_SetGeometry( poFeature, mGeom );
      }
    }
    else
    {
      OGR_F_SetGeometry( poFeature, createEmptyGeometry( mWkbType ) );
    }
  }
  return poFeature;
}

void QgsVectorFileWriter::resetMap( const QgsAttributeList &attributes )
{
  QMap<int, int> omap( mAttrIdxToOgrIdx );
  mAttrIdxToOgrIdx.clear();
  for ( int i = 0; i < attributes.size(); i++ )
  {
    mAttrIdxToOgrIdx.insert( attributes[i], omap[i] );
  }
}

bool QgsVectorFileWriter::writeFeature( OGRLayerH layer, OGRFeatureH feature )
{
  if ( OGR_L_CreateFeature( layer, feature ) != OGRERR_NONE )
  {
    mErrorMessage = QObject::tr( "Feature creation error (OGR error: %1)" ).arg( QString::fromUtf8( CPLGetLastErrorMsg() ) );
    mError = ErrFeatureWriteFailed;
    QgsMessageLog::logMessage( mErrorMessage, QObject::tr( "OGR" ) );
    OGR_F_Destroy( feature );
    return false;
  }
  return true;
}

QgsVectorFileWriter::~QgsVectorFileWriter()
{
  if ( mGeom )
  {
    OGR_G_DestroyGeometry( mGeom );
  }

  if ( mDS )
  {
    OGR_DS_Destroy( mDS );
  }

  if ( mOgrRef )
  {
    OSRDestroySpatialReference( mOgrRef );
  }
}

QgsVectorFileWriter::WriterError
QgsVectorFileWriter::writeAsVectorFormat( QgsVectorLayer* layer,
    const QString& fileName,
    const QString& fileEncoding,
    const QgsCoordinateReferenceSystem& destCRS,
    const QString& driverName,
    bool onlySelected,
    QString *errorMessage,
    const QStringList &datasourceOptions,
    const QStringList &layerOptions,
    bool skipAttributeCreation,
    QString *newFilename,
    SymbologyExport symbologyExport,
    double symbologyScale,
    const QgsRectangle* filterExtent,
    QgsWkbTypes::Type overrideGeometryType,
    bool forceMulti,
    bool includeZ,
    QgsAttributeList attributes,
    FieldValueConverter* fieldValueConverter )
{
  QgsCoordinateTransform ct;
  if ( destCRS.isValid() && layer )
  {
    ct = QgsCoordinateTransform( layer->crs(), destCRS );
  }

  QgsVectorFileWriter::WriterError error = writeAsVectorFormat( layer, fileName, fileEncoding, ct, driverName, onlySelected,
      errorMessage, datasourceOptions, layerOptions, skipAttributeCreation,
      newFilename, symbologyExport, symbologyScale, filterExtent,
      overrideGeometryType, forceMulti, includeZ, attributes,
      fieldValueConverter );
  return error;
}

QgsVectorFileWriter::WriterError QgsVectorFileWriter::writeAsVectorFormat( QgsVectorLayer* layer,
    const QString& fileName,
    const QString& fileEncoding,
    const QgsCoordinateTransform& ct,
    const QString& driverName,
    bool onlySelected,
    QString *errorMessage,
    const QStringList &datasourceOptions,
    const QStringList &layerOptions,
    bool skipAttributeCreation,
    QString *newFilename,
    SymbologyExport symbologyExport,
    double symbologyScale,
    const QgsRectangle* filterExtent,
    QgsWkbTypes::Type overrideGeometryType,
    bool forceMulti,
    bool includeZ,
    QgsAttributeList attributes,
    FieldValueConverter* fieldValueConverter )
{
  if ( !layer )
  {
    return ErrInvalidLayer;
  }

  bool shallTransform = false;
  QgsCoordinateReferenceSystem outputCRS;
  if ( ct.isValid() )
  {
    // This means we should transform
    outputCRS = ct.destinationCrs();
    shallTransform = true;
  }
  else
  {
    // This means we shouldn't transform, use source CRS as output (if defined)
    outputCRS = layer->crs();
  }

  QgsWkbTypes::Type destWkbType = layer->wkbType();
  if ( overrideGeometryType != QgsWkbTypes::Unknown )
  {
    destWkbType = QgsWkbTypes::flatType( overrideGeometryType );
    if ( QgsWkbTypes::hasZ( overrideGeometryType ) || includeZ )
      destWkbType = QgsWkbTypes::addZ( destWkbType );
  }
  if ( forceMulti )
  {
    destWkbType = QgsWkbTypes::multiType( destWkbType );
  }

  if ( skipAttributeCreation )
    attributes.clear();
  else if ( attributes.isEmpty() )
  {
    Q_FOREACH ( int idx, layer->attributeList() )
    {
      QgsField fld = layer->fields().at( idx );
      if ( layer->providerType() == "oracle" && fld.typeName().contains( "SDO_GEOMETRY" ) )
        continue;
      attributes.append( idx );
    }
  }

  QgsFields fields;
  if ( !attributes.isEmpty() )
  {
    Q_FOREACH ( int attrIdx, attributes )
    {
      fields.append( layer->fields().at( attrIdx ) );
    }
  }

  if ( layer->providerType() == "ogr" && layer->dataProvider() )
  {
    QStringList theURIParts = layer->dataProvider()->dataSourceUri().split( '|' );
    QString srcFileName = theURIParts[0];

    if ( QFile::exists( srcFileName ) && QFileInfo( fileName ).canonicalFilePath() == QFileInfo( srcFileName ).canonicalFilePath() )
    {
      if ( errorMessage )
        *errorMessage = QObject::tr( "Cannot overwrite a OGR layer in place" );
      return ErrCreateDataSource;
    }

    // Shapefiles might contain multi types although wkbType() only reports singles
    if ( layer->storageType() == "ESRI Shapefile" && !QgsWkbTypes::isMultiType( destWkbType ) )
    {
      QgsFeatureRequest req;
      if ( onlySelected )
      {
        req.setFilterFids( layer->selectedFeaturesIds() );
      }
      QgsFeatureIterator fit = layer->getFeatures( req );
      QgsFeature fet;

      while ( fit.nextFeature( fet ) )
      {
        if ( fet.hasGeometry() && QgsWkbTypes::isMultiType( fet.geometry().geometry()->wkbType() ) )
        {
          destWkbType = QgsWkbTypes::multiType( destWkbType );
          break;
        }
      }
    }
  }
  else if ( layer->providerType() == "spatialite" )
  {
    for ( int i = 0; i < fields.size(); i++ )
    {
      if ( fields.at( i ).type() == QVariant::LongLong )
      {
        QVariant min = layer->minimumValue( i );
        QVariant max = layer->maximumValue( i );
        if ( qMax( qAbs( min.toLongLong() ), qAbs( max.toLongLong() ) ) < INT_MAX )
        {
          fields[i].setType( QVariant::Int );
        }
      }
    }
  }

  QgsVectorFileWriter* writer =
    new QgsVectorFileWriter( fileName, fileEncoding, fields, destWkbType,
                             outputCRS, driverName, datasourceOptions, layerOptions,
                             newFilename, symbologyExport,
                             fieldValueConverter );
  writer->setSymbologyScaleDenominator( symbologyScale );

  if ( newFilename )
  {
    QgsDebugMsg( "newFilename = " + *newFilename );
  }

  // check whether file creation was successful
  WriterError err = writer->hasError();
  if ( err != NoError )
  {
    if ( errorMessage )
      *errorMessage = writer->errorMessage();
    delete writer;
    return err;
  }

  if ( errorMessage )
  {
    errorMessage->clear();
  }

  QgsFeature fet;

  //add possible attributes needed by renderer
  writer->addRendererAttributes( layer, attributes );

  QgsFeatureRequest req;
  if ( layer->wkbType() == QgsWkbTypes::NoGeometry )
  {
    req.setFlags( QgsFeatureRequest::NoGeometry );
  }
  req.setSubsetOfAttributes( attributes );
  if ( onlySelected )
    req.setFilterFids( layer->selectedFeaturesIds() );
  QgsFeatureIterator fit = layer->getFeatures( req );

  //create symbol table if needed
  if ( writer->symbologyExport() != NoSymbology )
  {
    //writer->createSymbolLayerTable( layer,  writer->mDS );
  }

  if ( writer->symbologyExport() == SymbolLayerSymbology )
  {
    QgsFeatureRenderer* r = layer->renderer();
    if ( r->capabilities() & QgsFeatureRenderer::SymbolLevels
         && r->usingSymbolLevels() )
    {
      QgsVectorFileWriter::WriterError error = writer->exportFeaturesSymbolLevels( layer, fit, ct, errorMessage );
      delete writer;
      return ( error == NoError ) ? NoError : ErrFeatureWriteFailed;
    }
  }

  int n = 0, errors = 0;

  //unit type
  QgsUnitTypes::DistanceUnit mapUnits = layer->crs().mapUnits();
  if ( ct.isValid() )
  {
    mapUnits = ct.destinationCrs().mapUnits();
  }

  writer->startRender( layer );

  // enabling transaction on databases that support it
  bool transactionsEnabled = true;

  if ( OGRERR_NONE != OGR_L_StartTransaction( writer->mLayer ) )
  {
    QgsDebugMsg( "Error when trying to enable transactions on OGRLayer." );
    transactionsEnabled = false;
  }

  writer->resetMap( attributes );
  // Reset mFields to layer fields, and not just exported fields
  writer->mFields = layer->fields();

  // write all features
  while ( fit.nextFeature( fet ) )
  {
    if ( shallTransform )
    {
      try
      {
        if ( fet.hasGeometry() )
        {
          QgsGeometry g = fet.geometry();
          g.transform( ct );
          fet.setGeometry( g );
        }
      }
      catch ( QgsCsException &e )
      {
        delete writer;

        QString msg = QObject::tr( "Failed to transform a point while drawing a feature with ID '%1'. Writing stopped. (Exception: %2)" )
                      .arg( fet.id() ).arg( e.what() );
        QgsLogger::warning( msg );
        if ( errorMessage )
          *errorMessage = msg;

        return ErrProjection;
      }
    }

    if ( fet.hasGeometry() && filterExtent && !fet.geometry().intersects( *filterExtent ) )
      continue;

    if ( attributes.size() < 1 && skipAttributeCreation )
    {
      fet.initAttributes( 0 );
    }

    if ( !writer->addFeature( fet, layer->renderer(), mapUnits ) )
    {
      WriterError err = writer->hasError();
      if ( err != NoError && errorMessage )
      {
        if ( errorMessage->isEmpty() )
        {
          *errorMessage = QObject::tr( "Feature write errors:" );
        }
        *errorMessage += '\n' + writer->errorMessage();
      }
      errors++;

      if ( errors > 1000 )
      {
        if ( errorMessage )
        {
          *errorMessage += QObject::tr( "Stopping after %1 errors" ).arg( errors );
        }

        n = -1;
        break;
      }
    }
    n++;
  }

  if ( transactionsEnabled )
  {
    if ( OGRERR_NONE != OGR_L_CommitTransaction( writer->mLayer ) )
    {
      QgsDebugMsg( "Error while committing transaction on OGRLayer." );
    }
  }

  writer->stopRender( layer );
  delete writer;

  if ( errors > 0 && errorMessage && n > 0 )
  {
    *errorMessage += QObject::tr( "\nOnly %1 of %2 features written." ).arg( n - errors ).arg( n );
  }

  return errors == 0 ? NoError : ErrFeatureWriteFailed;
}


bool QgsVectorFileWriter::deleteShapeFile( const QString& theFileName )
{
  QFileInfo fi( theFileName );
  QDir dir = fi.dir();

  QStringList filter;
  const char *suffixes[] = { ".shp", ".shx", ".dbf", ".prj", ".qix", ".qpj" };
  for ( std::size_t i = 0; i < sizeof( suffixes ) / sizeof( *suffixes ); i++ )
  {
    filter << fi.completeBaseName() + suffixes[i];
  }

  bool ok = true;
  Q_FOREACH ( const QString& file, dir.entryList( filter ) )
  {
    QFile f( dir.canonicalPath() + '/' + file );
    if ( !f.remove() )
    {
      QgsDebugMsg( QString( "Removing file %1 failed: %2" ).arg( file, f.errorString() ) );
      ok = false;
    }
  }

  return ok;
}

void QgsVectorFileWriter::setSymbologyScaleDenominator( double d )
{
  mSymbologyScaleDenominator = d;
  mRenderContext.setRendererScale( mSymbologyScaleDenominator );
}

QMap< QString, QString> QgsVectorFileWriter::supportedFiltersAndFormats()
{
  QMap<QString, QString> resultMap;

  QgsApplication::registerOgrDrivers();
  int const drvCount = OGRGetDriverCount();

  for ( int i = 0; i < drvCount; ++i )
  {
    OGRSFDriverH drv = OGRGetDriver( i );
    if ( drv )
    {
      QString drvName = OGR_Dr_GetName( drv );
      if ( OGR_Dr_TestCapability( drv, "CreateDataSource" ) != 0 )
      {
        QString filterString = filterForDriver( drvName );
        if ( filterString.isEmpty() )
          continue;

        resultMap.insert( filterString, drvName );
      }
    }
  }

  return resultMap;
}

QMap<QString, QString> QgsVectorFileWriter::ogrDriverList()
{
  QMap<QString, QString> resultMap;

  QgsApplication::registerOgrDrivers();
  int const drvCount = OGRGetDriverCount();

  QStringList writableDrivers;
  for ( int i = 0; i < drvCount; ++i )
  {
    OGRSFDriverH drv = OGRGetDriver( i );
    if ( drv )
    {
      QString drvName = OGR_Dr_GetName( drv );
      if ( OGR_Dr_TestCapability( drv, "CreateDataSource" ) != 0 )
      {
        // Add separate format for Mapinfo MIF (MITAB is OGR default)
        if ( drvName == "MapInfo File" )
        {
          writableDrivers << "MapInfo MIF";
        }
        else if ( drvName == "SQLite" )
        {
          // Unfortunately it seems that there is no simple way to detect if
          // OGR SQLite driver is compiled with SpatiaLite support.
          // We have HAVE_SPATIALITE in QGIS, but that may differ from OGR
          // http://lists.osgeo.org/pipermail/gdal-dev/2012-November/034580.html
          // -> test if creation failes
          QString option = "SPATIALITE=YES";
          char *options[2] = { CPLStrdup( option.toLocal8Bit().constData() ), nullptr };
          OGRSFDriverH poDriver;
          QgsApplication::registerOgrDrivers();
          poDriver = OGRGetDriverByName( drvName.toLocal8Bit().constData() );
          if ( poDriver )
          {
            OGRDataSourceH ds = OGR_Dr_CreateDataSource( poDriver, TO8F( QString( "/vsimem/spatialitetest.sqlite" ) ), options );
            if ( ds )
            {
              writableDrivers << "SpatiaLite";
              OGR_Dr_DeleteDataSource( poDriver, TO8F( QString( "/vsimem/spatialitetest.sqlite" ) ) );
              OGR_DS_Destroy( ds );
            }
          }
          CPLFree( options[0] );
        }
        else if ( drvName == "ESRI Shapefile" )
        {
          writableDrivers << "DBF file";
        }
        writableDrivers << drvName;
      }
    }
  }

  Q_FOREACH ( const QString& drvName, writableDrivers )
  {
    MetaData metadata;
    if ( driverMetadata( drvName, metadata ) && !metadata.trLongName.isEmpty() )
    {
      resultMap.insert( metadata.trLongName, drvName );
    }
  }

  return resultMap;
}

QString QgsVectorFileWriter::fileFilterString()
{
  QString filterString;
  QMap< QString, QString> driverFormatMap = supportedFiltersAndFormats();
  QMap< QString, QString>::const_iterator it = driverFormatMap.constBegin();
  for ( ; it != driverFormatMap.constEnd(); ++it )
  {
    if ( !filterString.isEmpty() )
      filterString += ";;";

    filterString += it.key();
  }
  return filterString;
}

QString QgsVectorFileWriter::filterForDriver( const QString& driverName )
{
  MetaData metadata;
  if ( !driverMetadata( driverName, metadata ) || metadata.trLongName.isEmpty() || metadata.glob.isEmpty() )
    return "";

  return metadata.trLongName + " [OGR] (" + metadata.glob.toLower() + ' ' + metadata.glob.toUpper() + ')';
}

QString QgsVectorFileWriter::convertCodecNameForEncodingOption( const QString &codecName )
{
  if ( codecName == "System" )
    return QString( "LDID/0" );

  QRegExp re = QRegExp( QString( "(CP|windows-|ISO[ -])(.+)" ), Qt::CaseInsensitive );
  if ( re.exactMatch( codecName ) )
  {
    QString c = re.cap( 2 ).remove( '-' );
    bool isNumber;
    c.toInt( &isNumber );
    if ( isNumber )
      return c;
  }
  return codecName;
}

void QgsVectorFileWriter::createSymbolLayerTable( QgsVectorLayer* vl,  const QgsCoordinateTransform& ct, OGRDataSourceH ds )
{
  if ( !vl || !ds )
  {
    return;
  }

  QgsFeatureRenderer* renderer = vl->renderer();
  if ( !renderer )
  {
    return;
  }

  //unit type
  QgsUnitTypes::DistanceUnit mapUnits = vl->crs().mapUnits();
  if ( ct.isValid() )
  {
    mapUnits = ct.destinationCrs().mapUnits();
  }

#if defined(GDAL_VERSION_NUM) && GDAL_VERSION_NUM >= 1700
  mSymbolLayerTable.clear();
  OGRStyleTableH ogrStyleTable = OGR_STBL_Create();
  OGRStyleMgrH styleManager = OGR_SM_Create( ogrStyleTable );

  //get symbols
  int nTotalLevels = 0;
  QgsSymbolList symbolList = renderer->symbols( mRenderContext );
  QgsSymbolList::iterator symbolIt = symbolList.begin();
  for ( ; symbolIt != symbolList.end(); ++symbolIt )
  {
    double mmsf = mmScaleFactor( mSymbologyScaleDenominator, ( *symbolIt )->outputUnit(), mapUnits );
    double musf = mapUnitScaleFactor( mSymbologyScaleDenominator, ( *symbolIt )->outputUnit(), mapUnits );

    int nLevels = ( *symbolIt )->symbolLayerCount();
    for ( int i = 0; i < nLevels; ++i )
    {
      mSymbolLayerTable.insert(( *symbolIt )->symbolLayer( i ), QString::number( nTotalLevels ) );
      OGR_SM_AddStyle( styleManager, QString::number( nTotalLevels ).toLocal8Bit(),
                       ( *symbolIt )->symbolLayer( i )->ogrFeatureStyle( mmsf, musf ).toLocal8Bit() );
      ++nTotalLevels;
    }
  }
  OGR_DS_SetStyleTableDirectly( ds, ogrStyleTable );
#endif
}

QgsVectorFileWriter::WriterError QgsVectorFileWriter::exportFeaturesSymbolLevels( QgsVectorLayer* layer, QgsFeatureIterator& fit,
    const QgsCoordinateTransform& ct, QString* errorMessage )
{
  if ( !layer )
    return ErrInvalidLayer;

  mRenderContext.expressionContext() = QgsExpressionContext();
  mRenderContext.expressionContext() << QgsExpressionContextUtils::globalScope()
  << QgsExpressionContextUtils::projectScope()
  << QgsExpressionContextUtils::layerScope( layer );

  QgsFeatureRenderer *renderer = layer->renderer();
  if ( !renderer )
    return ErrInvalidLayer;

  QHash< QgsSymbol*, QList<QgsFeature> > features;

  //unit type
  QgsUnitTypes::DistanceUnit mapUnits = layer->crs().mapUnits();
  if ( ct.isValid() )
  {
    mapUnits = ct.destinationCrs().mapUnits();
  }

  startRender( layer );

  //fetch features
  QgsFeature fet;
  QgsSymbol* featureSymbol = nullptr;
  while ( fit.nextFeature( fet ) )
  {
    if ( ct.isValid() )
    {
      try
      {
        if ( fet.hasGeometry() )
        {
          QgsGeometry g = fet.geometry();
          g.transform( ct );
          fet.setGeometry( g );
        }
      }
      catch ( QgsCsException &e )
      {
        QString msg = QObject::tr( "Failed to transform, writing stopped. (Exception: %1)" )
                      .arg( e.what() );
        QgsLogger::warning( msg );
        if ( errorMessage )
          *errorMessage = msg;

        return ErrProjection;
      }
    }
    mRenderContext.expressionContext().setFeature( fet );

    featureSymbol = renderer->symbolForFeature( fet, mRenderContext );
    if ( !featureSymbol )
    {
      continue;
    }

    QHash< QgsSymbol*, QList<QgsFeature> >::iterator it = features.find( featureSymbol );
    if ( it == features.end() )
    {
      it = features.insert( featureSymbol, QList<QgsFeature>() );
    }
    it.value().append( fet );
  }

  //find out order
  QgsSymbolLevelOrder levels;
  QgsSymbolList symbols = renderer->symbols( mRenderContext );
  for ( int i = 0; i < symbols.count(); i++ )
  {
    QgsSymbol* sym = symbols[i];
    for ( int j = 0; j < sym->symbolLayerCount(); j++ )
    {
      int level = sym->symbolLayer( j )->renderingPass();
      if ( level < 0 || level >= 1000 ) // ignore invalid levels
        continue;
      QgsSymbolLevelItem item( sym, j );
      while ( level >= levels.count() ) // append new empty levels
        levels.append( QgsSymbolLevel() );
      levels[level].append( item );
    }
  }

  int nErrors = 0;
  int nTotalFeatures = 0;

  //export symbol layers and symbology
  for ( int l = 0; l < levels.count(); l++ )
  {
    QgsSymbolLevel& level = levels[l];
    for ( int i = 0; i < level.count(); i++ )
    {
      QgsSymbolLevelItem& item = level[i];
      QHash< QgsSymbol*, QList<QgsFeature> >::iterator levelIt = features.find( item.symbol() );
      if ( levelIt == features.end() )
      {
        ++nErrors;
        continue;
      }

      double mmsf = mmScaleFactor( mSymbologyScaleDenominator, levelIt.key()->outputUnit(), mapUnits );
      double musf = mapUnitScaleFactor( mSymbologyScaleDenominator, levelIt.key()->outputUnit(), mapUnits );

      int llayer = item.layer();
      QList<QgsFeature>& featureList = levelIt.value();
      QList<QgsFeature>::iterator featureIt = featureList.begin();
      for ( ; featureIt != featureList.end(); ++featureIt )
      {
        ++nTotalFeatures;
        OGRFeatureH ogrFeature = createFeature( *featureIt );
        if ( !ogrFeature )
        {
          ++nErrors;
          continue;
        }

        QString styleString = levelIt.key()->symbolLayer( llayer )->ogrFeatureStyle( mmsf, musf );
        if ( !styleString.isEmpty() )
        {
          OGR_F_SetStyleString( ogrFeature, styleString.toLocal8Bit().constData() );
          if ( !writeFeature( mLayer, ogrFeature ) )
          {
            ++nErrors;
          }
        }
        OGR_F_Destroy( ogrFeature );
      }
    }
  }

  stopRender( layer );

  if ( nErrors > 0 && errorMessage )
  {
    *errorMessage += QObject::tr( "\nOnly %1 of %2 features written." ).arg( nTotalFeatures - nErrors ).arg( nTotalFeatures );
  }

  return ( nErrors > 0 ) ? QgsVectorFileWriter::ErrFeatureWriteFailed : QgsVectorFileWriter::NoError;
}

double QgsVectorFileWriter::mmScaleFactor( double scaleDenominator, QgsUnitTypes::RenderUnit symbolUnits, QgsUnitTypes::DistanceUnit mapUnits )
{
  if ( symbolUnits == QgsUnitTypes::RenderMillimeters )
  {
    return 1.0;
  }
  else
  {
    //conversion factor map units -> mm
    if ( mapUnits == QgsUnitTypes::DistanceMeters )
    {
      return 1000 / scaleDenominator;
    }

  }
  return 1.0; //todo: map units
}

double QgsVectorFileWriter::mapUnitScaleFactor( double scaleDenominator, QgsUnitTypes::RenderUnit symbolUnits, QgsUnitTypes::DistanceUnit mapUnits )
{
  if ( symbolUnits == QgsUnitTypes::RenderMapUnits )
  {
    return 1.0;
  }
  else
  {
    if ( symbolUnits == QgsUnitTypes::RenderMillimeters && mapUnits == QgsUnitTypes::DistanceMeters )
    {
      return scaleDenominator / 1000;
    }
  }
  return 1.0;
}

void QgsVectorFileWriter::startRender( QgsVectorLayer* vl )
{
  QgsFeatureRenderer* renderer = symbologyRenderer( vl );
  if ( !renderer )
  {
    return;
  }

  renderer->startRender( mRenderContext, vl->fields() );
}

void QgsVectorFileWriter::stopRender( QgsVectorLayer* vl )
{
  QgsFeatureRenderer* renderer = symbologyRenderer( vl );
  if ( !renderer )
  {
    return;
  }

  renderer->stopRender( mRenderContext );
}

QgsFeatureRenderer* QgsVectorFileWriter::symbologyRenderer( QgsVectorLayer* vl ) const
{
  if ( mSymbologyExport == NoSymbology )
  {
    return nullptr;
  }
  if ( !vl )
  {
    return nullptr;
  }

  return vl->renderer();
}

void QgsVectorFileWriter::addRendererAttributes( QgsVectorLayer* vl, QgsAttributeList& attList )
{
  QgsFeatureRenderer* renderer = symbologyRenderer( vl );
  if ( renderer )
  {
    QList<QString> rendererAttributes = renderer->usedAttributes();
    for ( int i = 0; i < rendererAttributes.size(); ++i )
    {
      int index = vl->fieldNameIndex( rendererAttributes.at( i ) );
      if ( index != -1 )
      {
        attList.push_back( vl->fieldNameIndex( rendererAttributes.at( i ) ) );
      }
    }
  }
}

QStringList QgsVectorFileWriter::concatenateOptions( const QMap<QString, QgsVectorFileWriter::Option*>& options )
{
  QStringList list;
  QMap<QString, QgsVectorFileWriter::Option*>::ConstIterator it;

  for ( it = options.constBegin(); it != options.constEnd(); ++it )
  {
    QgsVectorFileWriter::Option *option = it.value();
    switch ( option->type )
    {
      case QgsVectorFileWriter::Int:
      {
        QgsVectorFileWriter::IntOption *opt = dynamic_cast<QgsVectorFileWriter::IntOption*>( option );
        if ( opt )
        {
          list.append( QString( "%1=%2" ).arg( it.key() ).arg( opt->defaultValue ) );
        }
        break;
      }

      case QgsVectorFileWriter::Set:
      {
        QgsVectorFileWriter::SetOption *opt = dynamic_cast<QgsVectorFileWriter::SetOption*>( option );
        if ( opt && !opt->defaultValue.isEmpty() )
        {
          list.append( QString( "%1=%2" ).arg( it.key(), opt->defaultValue ) );
        }
        break;
      }

      case QgsVectorFileWriter::String:
      {
        QgsVectorFileWriter::StringOption *opt = dynamic_cast<QgsVectorFileWriter::StringOption*>( option );
        if ( opt )
        {
          list.append( QString( "%1=%2" ).arg( it.key(), opt->defaultValue ) );
        }
        break;
      }

      case QgsVectorFileWriter::Hidden:
        QgsVectorFileWriter::HiddenOption *opt = dynamic_cast<QgsVectorFileWriter::HiddenOption*>( option );
        if ( opt )
        {
          list.append( QString( "%1=%2" ).arg( it.key(), opt->mValue ) );
        }
        break;
    }
  }

  return list;
}
