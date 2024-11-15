
/***************************************************************************
  qgslabelingengine.cpp
  --------------------------------------
  Date                 : September 2015
  Copyright            : (C) 2015 by Martin Dobias
  Email                : wonder dot sk at gmail dot com
 ***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include "qgslabelingengine.h"

#include "qgslogger.h"

#include "feature.h"
#include "labelposition.h"
#include "layer.h"
#include "pal.h"
#include "problem.h"
#include "qgsrendercontext.h"
#include "qgsmaplayer.h"
#include "qgssymbol.h"
#include "qgsexpressioncontextutils.h"
#include "qgsvectorlayerlabelprovider.h"
#include "qgslabelingresults.h"
#include "qgsfillsymbol.h"

// helper function for checking for job cancellation within PAL
static bool _palIsCanceled( void *ctx )
{
  return ( reinterpret_cast< QgsRenderContext * >( ctx ) )->renderingStopped();
}

/**
 * \ingroup core
 * \class QgsLabelSorter
 * \brief Helper class for sorting labels into correct draw order
 */
class QgsLabelSorter
{
  public:

    explicit QgsLabelSorter( const QgsMapSettings &mapSettings )
      : mMapSettings( mapSettings )
    {}

    bool operator()( pal::LabelPosition *lp1, pal::LabelPosition *lp2 ) const
    {
      QgsLabelFeature *lf1 = lp1->getFeaturePart()->feature();
      QgsLabelFeature *lf2 = lp2->getFeaturePart()->feature();

      if ( !qgsDoubleNear( lf1->zIndex(), lf2->zIndex() ) )
        return lf1->zIndex() < lf2->zIndex();

      //equal z-index, so fallback to respecting layer render order
      QStringList layerIds = mMapSettings.layerIds();
      int layer1Pos = layerIds.indexOf( lf1->provider()->layerId() );
      int layer2Pos = layerIds.indexOf( lf2->provider()->layerId() );
      if ( layer1Pos != layer2Pos && layer1Pos >= 0 && layer2Pos >= 0 )
        return layer1Pos > layer2Pos; //higher positions are rendered first

      //same layer, so render larger labels first
      return lf1->size().width() * lf1->size().height() > lf2->size().width() * lf2->size().height();
    }

  private:

    const QgsMapSettings &mMapSettings;
};

//
// QgsLabelingEngine
//

QgsLabelingEngine::QgsLabelingEngine()
  : mResults( new QgsLabelingResults )
{}

QgsLabelingEngine::~QgsLabelingEngine()
{
  qDeleteAll( mProviders );
  qDeleteAll( mSubProviders );
}

void QgsLabelingEngine::setMapSettings( const QgsMapSettings &mapSettings )
{
  mMapSettings = mapSettings;
  if ( mResults )
    mResults->setMapSettings( mapSettings );
}

QList< QgsMapLayer * > QgsLabelingEngine::participatingLayers() const
{
  QList< QgsMapLayer * > layers;

  // try to return layers sorted in the desired z order for rendering
  QList< QgsAbstractLabelProvider * > providersByZ = mProviders;
  std::sort( providersByZ.begin(), providersByZ.end(),
             []( const QgsAbstractLabelProvider * a, const QgsAbstractLabelProvider * b ) -> bool
  {
    const QgsVectorLayerLabelProvider *providerA = dynamic_cast<const QgsVectorLayerLabelProvider *>( a );
    const QgsVectorLayerLabelProvider *providerB = dynamic_cast<const QgsVectorLayerLabelProvider *>( b );

    if ( providerA && providerB )
    {
      return providerA->settings().zIndex < providerB->settings().zIndex ;
    }
    return false;
  } );

  QList< QgsAbstractLabelProvider * > subProvidersByZ = mSubProviders;
  std::sort( subProvidersByZ.begin(), subProvidersByZ.end(),
             []( const QgsAbstractLabelProvider * a, const QgsAbstractLabelProvider * b ) -> bool
  {
    const QgsVectorLayerLabelProvider *providerA = dynamic_cast<const QgsVectorLayerLabelProvider *>( a );
    const QgsVectorLayerLabelProvider *providerB = dynamic_cast<const QgsVectorLayerLabelProvider *>( b );

    if ( providerA && providerB )
    {
      return providerA->settings().zIndex < providerB->settings().zIndex ;
    }
    return false;
  } );

  for ( QgsAbstractLabelProvider *provider : std::as_const( providersByZ ) )
  {
    if ( provider->layer() && !layers.contains( provider->layer() ) )
      layers << provider->layer();
  }
  for ( QgsAbstractLabelProvider *provider : std::as_const( subProvidersByZ ) )
  {
    if ( provider->layer() && !layers.contains( provider->layer() ) )
      layers << provider->layer();
  }
  return layers;
}

QStringList QgsLabelingEngine::participatingLayerIds() const
{
  QStringList layers;

  // try to return layers sorted in the desired z order for rendering
  QList< QgsAbstractLabelProvider * > providersByZ = mProviders;
  std::sort( providersByZ.begin(), providersByZ.end(),
             []( const QgsAbstractLabelProvider * a, const QgsAbstractLabelProvider * b ) -> bool
  {
    const QgsVectorLayerLabelProvider *providerA = dynamic_cast<const QgsVectorLayerLabelProvider *>( a );
    const QgsVectorLayerLabelProvider *providerB = dynamic_cast<const QgsVectorLayerLabelProvider *>( b );

    if ( providerA && providerB )
    {
      return providerA->settings().zIndex < providerB->settings().zIndex ;
    }
    return false;
  } );

  QList< QgsAbstractLabelProvider * > subProvidersByZ = mSubProviders;
  std::sort( subProvidersByZ.begin(), subProvidersByZ.end(),
             []( const QgsAbstractLabelProvider * a, const QgsAbstractLabelProvider * b ) -> bool
  {
    const QgsVectorLayerLabelProvider *providerA = dynamic_cast<const QgsVectorLayerLabelProvider *>( a );
    const QgsVectorLayerLabelProvider *providerB = dynamic_cast<const QgsVectorLayerLabelProvider *>( b );

    if ( providerA && providerB )
    {
      return providerA->settings().zIndex < providerB->settings().zIndex ;
    }
    return false;
  } );

  for ( QgsAbstractLabelProvider *provider : std::as_const( providersByZ ) )
  {
    if ( !layers.contains( provider->layerId() ) )
      layers << provider->layerId();
  }
  for ( QgsAbstractLabelProvider *provider : std::as_const( subProvidersByZ ) )
  {
    if ( !layers.contains( provider->layerId() ) )
      layers << provider->layerId();
  }
  return layers;
}

void QgsLabelingEngine::addProvider( QgsAbstractLabelProvider *provider )
{
  provider->setEngine( this );
  mProviders << provider;
}

void QgsLabelingEngine::removeProvider( QgsAbstractLabelProvider *provider )
{
  int idx = mProviders.indexOf( provider );
  if ( idx >= 0 )
  {
    delete mProviders.takeAt( idx );
  }
}

void QgsLabelingEngine::processProvider( QgsAbstractLabelProvider *provider, QgsRenderContext &context, pal::Pal &p )
{
  QgsAbstractLabelProvider::Flags flags = provider->flags();

  // create the pal layer
  pal::Layer *l = p.addLayer( provider,
                              provider->name(),
                              provider->placement(),
                              provider->priority(),
                              true,
                              flags.testFlag( QgsAbstractLabelProvider::DrawLabels ),
                              flags.testFlag( QgsAbstractLabelProvider::DrawAllLabels ) );

  // set whether adjacent lines should be merged
  l->setMergeConnectedLines( flags.testFlag( QgsAbstractLabelProvider::MergeConnectedLines ) );

  // set obstacle type
  l->setObstacleType( provider->obstacleType() );

  // set whether location of centroid must be inside of polygons
  l->setCentroidInside( flags.testFlag( QgsAbstractLabelProvider::CentroidMustBeInside ) );

  // set how to show upside-down labels
  pal::Layer::UpsideDownLabels upsdnlabels = pal::Layer::ShowAll;
  switch ( provider->upsidedownLabels() )
  {
    case QgsPalLayerSettings::Upright:
      upsdnlabels = pal::Layer::Upright;
      break;
    case QgsPalLayerSettings::ShowDefined:
      upsdnlabels = pal::Layer::ShowDefined;
      break;
    case QgsPalLayerSettings::ShowAll:
      upsdnlabels = pal::Layer::ShowAll;
      break;
  }
  l->setUpsidedownLabels( upsdnlabels );


  const QList<QgsLabelFeature *> features = provider->labelFeatures( context );

  for ( QgsLabelFeature *feature : features )
  {
    try
    {
      l->registerFeature( feature );
    }
    catch ( std::exception &e )
    {
      Q_UNUSED( e )
      QgsDebugMsgLevel( QStringLiteral( "Ignoring feature %1 due PAL exception:" ).arg( feature->id() ) + QString::fromLatin1( e.what() ), 4 );
      continue;
    }
  }

  // any sub-providers?
  const auto subproviders = provider->subProviders();
  for ( QgsAbstractLabelProvider *subProvider : subproviders )
  {
    mSubProviders << subProvider;
    processProvider( subProvider, context, p );
  }
}

void QgsLabelingEngine::registerLabels( QgsRenderContext &context )
{
  const QgsLabelingEngineSettings &settings = mMapSettings.labelingEngineSettings();

  mPal = std::make_unique< pal::Pal >();

  mPal->setMaximumLineCandidatesPerMapUnit( settings.maximumLineCandidatesPerCm() / context.convertToMapUnits( 10, QgsUnitTypes::RenderMillimeters ) );
  mPal->setMaximumPolygonCandidatesPerMapUnitSquared( settings.maximumPolygonCandidatesPerCmSquared() / std::pow( context.convertToMapUnits( 10, QgsUnitTypes::RenderMillimeters ), 2 ) );

  mPal->setShowPartialLabels( settings.testFlag( QgsLabelingEngineSettings::UsePartialCandidates ) );
  mPal->setPlacementVersion( settings.placementVersion() );

  // for each provider: get labels and register them in PAL
  for ( QgsAbstractLabelProvider *provider : std::as_const( mProviders ) )
  {
    std::unique_ptr< QgsExpressionContextScopePopper > layerScopePopper;
    if ( provider->layerExpressionContextScope() )
    {
      layerScopePopper = std::make_unique< QgsExpressionContextScopePopper >( context.expressionContext(), new QgsExpressionContextScope( *provider->layerExpressionContextScope() ) );
    }
    processProvider( provider, context, *mPal );
  }
}

#include "qgsspatialiteutils.h"
#include "qgsproject.h"
#include "sqlite3.h"

struct SpatialiteSession
{
    spatialite_database_unique_ptr db;
    int version = -1;
};

static int sqlite_bbox_exists(void* data,int argc, char** argv, char**)
{
    QSet<QString>* keys = static_cast<QSet<QString>*>(data);
    if(argc > 0) {
        keys->insert(argv[0]);
    }
    return 0;
}

static void solveWmtsProblems(pal::Problem* problems, int level, const QgsProject* project,
    QList<pal::LabelPosition*>* labels, QList<pal::LabelPosition*>* unLabels)
{
    static QMap<QString, SpatialiteSession*> dbSessionPool;
    static QString boxIntersectSql("select key from bbox where level = %1 and "
                               "ST_Intersects(bbox, BuildMBR(%2, %3, %4, %5));");
    static QString boxInsertSql(
        "insert into bbox values('%1', %2, SetSRID( BuildMBR(%3, %4, %5, %6), 0));");

    QString wsPath = QFileInfo(project->fileName()).absolutePath();
    QString dbFile = QDir(wsPath).filePath("bbox.db");

    int lidx = wsPath.lastIndexOf("-");
    if (lidx < 0) {
        return;
    }

    QString dbKey = wsPath.left(lidx);
    QString sVersion = wsPath.right(wsPath.size() - lidx - 1);
    int version = sVersion.toInt();

    auto it = dbSessionPool.find(dbKey);
    SpatialiteSession* session = nullptr;
    if (it != dbSessionPool.end()) {
        if (it.value()->version != version) {
            delete it.value();
            dbSessionPool.erase(it);
        } else {
            session = it.value();
        }
    }
    if (session == nullptr) {
        spatialite_database_unique_ptr spatialite;
        if (SQLITE_OK != spatialite.open(dbFile)) {
            return;
        }
        session = new SpatialiteSession();
        session->db = std::move(spatialite);
        session->version = version;

        dbSessionPool[dbKey] = session;
    }

    QMap<QString, pal::LabelPosition*> uniqueLables;
    pal::LabelPosition* p = nullptr;
    double cx = 0.0, cy = 0.0;
    for (int i = 0; i < static_cast<int>(problems->featureCount()); i++) {
        for (int j = 0; j < problems->featureCandidateCount(i); j++) {
            p = problems->featureCandidate(i, j);
            QString txt = p->getFeaturePart()->feature()->labelText();
            p->getFeaturePart()->getCentroid(cx, cy);
            txt = QString("%1-%2-%3")
                .arg(qlonglong (cx * 1E10), 20, 10, QLatin1Char('0'))
                .arg(qlonglong (cy * 1E10), 20, 10, QLatin1Char('0'))
                .arg(txt);
            uniqueLables[txt] = p;
        }
    }

    int ret = sqlite3_exec(session->db.get(), "BEGIN", nullptr, nullptr, nullptr);
    if (ret != SQLITE_OK) {
        return;
    }
    // qDebug() << "start" << QDateTime::currentDateTime().toMSecsSinceEpoch();

    QSet<QString> intersectKeys;
    for (auto it = uniqueLables.begin(); it != uniqueLables.end(); it++) {
        const QString& key = it.key();
        p = it.value();
        QString sql = boxIntersectSql.arg(level)
                            .arg(p->getX(), 0, 'g', 15)
                            .arg(p->getY(), 0, 'g', 15)
                            .arg(p->getX() + p->getWidth(), 0, 'g', 15)
                            .arg(p->getY() + p->getHeight(), 0, 'g', 15);
        intersectKeys.clear();
        if (SQLITE_OK
            != sqlite3_exec(session->db.get(), sql.toUtf8(), sqlite_bbox_exists, &intersectKeys, nullptr)) {
            qDebug() << "Find intersect item error";
            break;
        }
        if(intersectKeys.empty()) {
            labels->push_back(p);
            sql = boxInsertSql.arg(key)
                        .arg(level)
                        .arg(p->getX(), 0, 'g', 15)
                        .arg(p->getY(), 0, 'g', 15)
                        .arg(p->getX() + p->getWidth(), 0, 'g', 15)
                        .arg(p->getY() + p->getHeight(), 0, 'g', 15);
            ret = sqlite3_exec(session->db.get(), sql.toUtf8(), nullptr, nullptr, nullptr);
            if (SQLITE_OK != ret && SQLITE_CONSTRAINT != ret) {
                qDebug() << "Insert item error";
                break;
            }
        } else {
            if (intersectKeys.contains(key)) {
                labels->push_back(p);
            } else if(unLabels) {
                unLabels->push_back(p);
            }
        }
    }

    ret = sqlite3_exec(session->db.get(), "COMMIT", nullptr, nullptr, nullptr);
    // qDebug() << "over:" << labels->size() << " " <<
    QDateTime::currentDateTime().toMSecsSinceEpoch();
}

void QgsLabelingEngine::solve( QgsRenderContext &context )
{
  Q_ASSERT( mPal.get() );

  // NOW DO THE LAYOUT (from QgsPalLabeling::drawLabeling)
  const QgsLabelingEngineSettings &settings = mMapSettings.labelingEngineSettings();

  QPainter *painter = context.painter();

  QgsRectangle r1 = mMapSettings.visibleExtent();
  r1.grow( mMapSettings.extentBuffer() );
  QgsGeometry extentGeom = QgsGeometry::fromRect( r1 );

  QPolygonF visiblePoly = mMapSettings.visiblePolygonWithBuffer();
  visiblePoly.append( visiblePoly.at( 0 ) ); //close polygon

  // get map label boundary geometry - if one hasn't been explicitly set, we use the whole of the map's visible polygon
  QgsGeometry mapBoundaryGeom = !mMapSettings.labelBoundaryGeometry().isNull() ? mMapSettings.labelBoundaryGeometry() : QgsGeometry::fromQPolygonF( visiblePoly );

  // label blocking regions work by "chopping away" those regions from the permissible labeling area
  const QList< QgsLabelBlockingRegion > blockingRegions = mMapSettings.labelBlockingRegions();
  for ( const QgsLabelBlockingRegion &region : blockingRegions )
  {
    mapBoundaryGeom = mapBoundaryGeom.difference( region.geometry );
  }

  if ( settings.flags() & QgsLabelingEngineSettings::DrawCandidates )
  {
    // draw map boundary
    QgsFeature f;
    f.setGeometry( mapBoundaryGeom );
    QVariantMap properties;
    properties.insert( QStringLiteral( "style" ), QStringLiteral( "no" ) );
    properties.insert( QStringLiteral( "style_border" ), QStringLiteral( "solid" ) );
    properties.insert( QStringLiteral( "color_border" ), QStringLiteral( "#0000ff" ) );
    properties.insert( QStringLiteral( "width_border" ), QStringLiteral( "0.3" ) );
    properties.insert( QStringLiteral( "joinstyle" ), QStringLiteral( "miter" ) );
    std::unique_ptr< QgsFillSymbol > boundarySymbol( QgsFillSymbol::createSimple( properties ) );
    boundarySymbol->startRender( context );
    boundarySymbol->renderFeature( f, context );
    boundarySymbol->stopRender( context );
  }

  if ( !qgsDoubleNear( mMapSettings.rotation(), 0.0 ) )
  {
    //PAL features are prerotated, so extent also needs to be unrotated
    extentGeom.rotate( -mMapSettings.rotation(), mMapSettings.visibleExtent().center() );
    // yes - this is rotated in the opposite direction... phew, this is confusing!
    mapBoundaryGeom.rotate( mMapSettings.rotation(), mMapSettings.visibleExtent().center() );
  }

  QgsRectangle extent = extentGeom.boundingBox();

  mPal->registerCancellationCallback( &_palIsCanceled, reinterpret_cast< void * >( &context ) );

  QElapsedTimer t;
  t.start();

  // do the labeling itself
  try
  {
    mProblem = mPal->extractProblem( extent, mapBoundaryGeom );
  }
  catch ( std::exception &e )
  {
    Q_UNUSED( e )
    QgsDebugMsgLevel( "PAL EXCEPTION :-( " + QString::fromLatin1( e.what() ), 4 );
    return;
  }

  if ( context.renderingStopped() )
  {
    return; // it has been canceled
  }

#if 1 // XXX strk
  // features are pre-rotated but not scaled/translated,
  // so we only disable rotation here. Ideally, they'd be
  // also pre-scaled/translated, as suggested here:
  // https://github.com/qgis/QGIS/issues/20071
  QgsMapToPixel xform = mMapSettings.mapToPixel();
  xform.setMapRotation( 0, 0, 0 );
#else
  const QgsMapToPixel &xform = mMapSettings->mapToPixel();
#endif

  // draw rectangles with all candidates
  // this is done before actual solution of the problem
  // before number of candidates gets reduced
  // TODO mCandidates.clear();
  if ( settings.testFlag( QgsLabelingEngineSettings::DrawCandidates ) && mProblem )
  {
    painter->setBrush( Qt::NoBrush );
    for ( int i = 0; i < static_cast< int >( mProblem->featureCount() ); i++ )
    {
      for ( int j = 0; j < mProblem->featureCandidateCount( i ); j++ )
      {
        pal::LabelPosition *lp = mProblem->featureCandidate( i, j );

        QgsPalLabeling::drawLabelCandidateRect( lp, painter, &xform );
      }
    }
  }

  // find the solution
  bool unlabel = settings.testFlag( QgsLabelingEngineSettings::DrawUnplacedLabels ) || settings.testFlag( QgsLabelingEngineSettings::CollectUnplacedLabels );

  if(mMapSettings.mWmtsMatrix >= 0) {
    solveWmtsProblems(mProblem.get(), mMapSettings.mWmtsMatrix, mMapSettings.mProject, &mLabels, unlabel ? &mUnlabeled : nullptr);
  } else {
    mLabels = mPal->solveProblem( mProblem.get(),
                                    settings.testFlag( QgsLabelingEngineSettings::UseAllLabels ),
                                    unlabel ? &mUnlabeled : nullptr );
  }

  // sort labels
  std::sort( mLabels.begin(), mLabels.end(), QgsLabelSorter( mMapSettings ) );

  QgsDebugMsgLevel( QStringLiteral( "LABELING work:  %1 ms ... labels# %2" ).arg( t.elapsed() ).arg( mLabels.size() ), 4 );
}

void QgsLabelingEngine::drawLabels( QgsRenderContext &context, const QString &layerId )
{
  QElapsedTimer t;
  t.start();

  const QgsLabelingEngineSettings &settings = mMapSettings.labelingEngineSettings();

  context.setPainterFlagsUsingContext();
  QPainter *painter = context.painter();

  // prepare for rendering
  for ( QgsAbstractLabelProvider *provider : std::as_const( mProviders ) )
  {
    if ( !layerId.isEmpty() && provider->layerId() != layerId )
      continue;

    // provider will require the correct layer scope for expression preparation - at this stage, the existing expression context
    // only contains generic scopes
    QgsExpressionContextScopePopper popper( context.expressionContext(), provider->layerExpressionContextScope() ? new QgsExpressionContextScope( *provider->layerExpressionContextScope() ) : new QgsExpressionContextScope() );

    QgsScopedRenderContextReferenceScaleOverride referenceScaleOverride( context, provider->layerReferenceScale() );
    provider->startRender( context );
  }

  QgsExpressionContextScope *symbolScope = new QgsExpressionContextScope();
  std::unique_ptr< QgsExpressionContextScopePopper > symbolScopePopper = std::make_unique< QgsExpressionContextScopePopper >( context.expressionContext(), symbolScope );

  // draw label backgrounds
  for ( pal::LabelPosition *label : std::as_const( mLabels ) )
  {
    if ( context.renderingStopped() )
      break;

    QgsLabelFeature *lf = label->getFeaturePart()->feature();
    if ( !lf )
    {
      continue;
    }

    if ( !layerId.isEmpty() && lf->provider()->layerId() != layerId )
      continue;

    context.expressionContext().setFeature( lf->feature() );
    context.expressionContext().setFields( lf->feature().fields() );

    QgsScopedRenderContextReferenceScaleOverride referenceScaleOverride( context, lf->provider()->layerReferenceScale() );

    if ( lf->symbol() )
    {
      symbolScope = QgsExpressionContextUtils::updateSymbolScope( lf->symbol(), symbolScope );
    }
    lf->provider()->drawLabelBackground( context, label );
  }

  // draw the labels
  for ( pal::LabelPosition *label : std::as_const( mLabels ) )
  {
    if ( context.renderingStopped() )
      break;

    QgsLabelFeature *lf = label->getFeaturePart()->feature();
    if ( !lf )
    {
      continue;
    }

    if ( !layerId.isEmpty() && lf->provider()->layerId() != layerId )
      continue;

    context.expressionContext().setFeature( lf->feature() );
    context.expressionContext().setFields( lf->feature().fields() );

    QgsScopedRenderContextReferenceScaleOverride referenceScaleOverride( context, lf->provider()->layerReferenceScale() );
    if ( lf->symbol() )
    {
      symbolScope = QgsExpressionContextUtils::updateSymbolScope( lf->symbol(), symbolScope );
    }
    lf->provider()->drawLabel( context, label );
    // finished with symbol -- we can't keep it around after this, it may be deleted
    lf->setSymbol( nullptr );
  }

  // draw unplaced labels. These are always rendered on top
  if ( settings.testFlag( QgsLabelingEngineSettings::DrawUnplacedLabels ) || settings.testFlag( QgsLabelingEngineSettings::CollectUnplacedLabels ) )
  {
    for ( pal::LabelPosition *label : std::as_const( mUnlabeled ) )
    {
      if ( context.renderingStopped() )
        break;
      QgsLabelFeature *lf = label->getFeaturePart()->feature();
      if ( !lf )
      {
        continue;
      }

      if ( !layerId.isEmpty() && lf->provider()->layerId() != layerId )
        continue;

      context.expressionContext().setFeature( lf->feature() );
      context.expressionContext().setFields( lf->feature().fields() );

      QgsScopedRenderContextReferenceScaleOverride referenceScaleOverride( context, lf->provider()->layerReferenceScale() );
      if ( lf->symbol() )
      {
        symbolScope = QgsExpressionContextUtils::updateSymbolScope( lf->symbol(), symbolScope );
      }
      lf->provider()->drawUnplacedLabel( context, label );
      // finished with symbol -- we can't keep it around after this, it may be deleted
      lf->setSymbol( nullptr );
    }
  }

  symbolScopePopper.reset();

  // cleanup
  for ( QgsAbstractLabelProvider *provider : std::as_const( mProviders ) )
  {
    if ( !layerId.isEmpty() && provider->layerId() != layerId )
      continue;

    provider->stopRender( context );
  }

  // Reset composition mode for further drawing operations
  painter->setCompositionMode( QPainter::CompositionMode_SourceOver );

  QgsDebugMsgLevel( QStringLiteral( "LABELING draw:  %1 ms" ).arg( t.elapsed() ), 4 );
}

void QgsLabelingEngine::cleanup()
{
  mUnlabeled.clear();
  mLabels.clear();
  mProblem.reset();
  mPal.reset();
}

QgsLabelingResults *QgsLabelingEngine::takeResults()
{
  return mResults.release();
}


//
//  QgsDefaultLabelingEngine
//

QgsDefaultLabelingEngine::QgsDefaultLabelingEngine()
  : QgsLabelingEngine()
{

}

void QgsDefaultLabelingEngine::run( QgsRenderContext &context )
{
  registerLabels( context );
  if ( context.renderingStopped() )
  {
    cleanup();
    return; // it has been canceled
  }

  solve( context );
  if ( context.renderingStopped() )
  {
    cleanup();
    return;
  }

  drawLabels( context );
  cleanup();
}


//
//  QgsStagedRenderLabelingEngine
//

QgsStagedRenderLabelingEngine::QgsStagedRenderLabelingEngine()
  : QgsLabelingEngine()
{

}

void QgsStagedRenderLabelingEngine::run( QgsRenderContext &context )
{
  registerLabels( context );
  if ( context.renderingStopped() )
  {
    cleanup();
    return; // it has been canceled
  }

  solve( context );
  if ( context.renderingStopped() )
  {
    cleanup();
    return;
  }
}


void QgsStagedRenderLabelingEngine::renderLabelsForLayer( QgsRenderContext &context, const QString &layerId )
{
  drawLabels( context, layerId );
}

void QgsStagedRenderLabelingEngine::finalize()
{
  cleanup();
}


////

QgsAbstractLabelProvider *QgsLabelFeature::provider() const
{
  return mLayer ? mLayer->provider() : nullptr;

}

QgsAbstractLabelProvider::QgsAbstractLabelProvider( QgsMapLayer *layer, const QString &providerId )
  : mLayerId( layer ? layer->id() : QString() )
  , mLayer( layer )
  , mProviderId( providerId )
  , mFlags( DrawLabels )
  , mPlacement( QgsPalLayerSettings::AroundPoint )
  , mPriority( 0.5 )
  , mUpsidedownLabels( QgsPalLayerSettings::Upright )
{
  if ( QgsVectorLayer *vl = qobject_cast< QgsVectorLayer * >( layer ) )
  {
    mLayerExpressionContextScope.reset( vl->createExpressionContextScope() );
    if ( const QgsFeatureRenderer *renderer = vl->renderer() )
      mLayerReferenceScale = renderer->referenceScale();
  }
}

void QgsAbstractLabelProvider::drawUnplacedLabel( QgsRenderContext &, pal::LabelPosition * ) const
{

}

void QgsAbstractLabelProvider::drawLabelBackground( QgsRenderContext &, pal::LabelPosition * ) const
{

}

void QgsAbstractLabelProvider::startRender( QgsRenderContext &context )
{
  const auto subproviders = subProviders();
  for ( QgsAbstractLabelProvider *subProvider : subproviders )
  {
    subProvider->startRender( context );
  }
}

void QgsAbstractLabelProvider::stopRender( QgsRenderContext &context )
{
  const auto subproviders = subProviders();
  for ( QgsAbstractLabelProvider *subProvider : subproviders )
  {
    subProvider->stopRender( context );
  }
}

QgsExpressionContextScope *QgsAbstractLabelProvider::layerExpressionContextScope() const
{
  return mLayerExpressionContextScope.get();
}

//
// QgsLabelingUtils
//

QString QgsLabelingUtils::encodePredefinedPositionOrder( const QVector<QgsPalLayerSettings::PredefinedPointPosition> &positions )
{
  QStringList predefinedOrderString;
  const auto constPositions = positions;
  for ( QgsPalLayerSettings::PredefinedPointPosition position : constPositions )
  {
    switch ( position )
    {
      case QgsPalLayerSettings::TopLeft:
        predefinedOrderString << QStringLiteral( "TL" );
        break;
      case QgsPalLayerSettings::TopSlightlyLeft:
        predefinedOrderString << QStringLiteral( "TSL" );
        break;
      case QgsPalLayerSettings::TopMiddle:
        predefinedOrderString << QStringLiteral( "T" );
        break;
      case QgsPalLayerSettings::TopSlightlyRight:
        predefinedOrderString << QStringLiteral( "TSR" );
        break;
      case QgsPalLayerSettings::TopRight:
        predefinedOrderString << QStringLiteral( "TR" );
        break;
      case QgsPalLayerSettings::MiddleLeft:
        predefinedOrderString << QStringLiteral( "L" );
        break;
      case QgsPalLayerSettings::MiddleRight:
        predefinedOrderString << QStringLiteral( "R" );
        break;
      case QgsPalLayerSettings::BottomLeft:
        predefinedOrderString << QStringLiteral( "BL" );
        break;
      case QgsPalLayerSettings::BottomSlightlyLeft:
        predefinedOrderString << QStringLiteral( "BSL" );
        break;
      case QgsPalLayerSettings::BottomMiddle:
        predefinedOrderString << QStringLiteral( "B" );
        break;
      case QgsPalLayerSettings::BottomSlightlyRight:
        predefinedOrderString << QStringLiteral( "BSR" );
        break;
      case QgsPalLayerSettings::BottomRight:
        predefinedOrderString << QStringLiteral( "BR" );
        break;
    }
  }
  return predefinedOrderString.join( ',' );
}

QVector<QgsPalLayerSettings::PredefinedPointPosition> QgsLabelingUtils::decodePredefinedPositionOrder( const QString &positionString )
{
  QVector<QgsPalLayerSettings::PredefinedPointPosition> result;
  const QStringList predefinedOrderList = positionString.split( ',' );
  result.reserve( predefinedOrderList.size() );
  for ( const QString &position : predefinedOrderList )
  {
    QString cleaned = position.trimmed().toUpper();
    if ( cleaned == QLatin1String( "TL" ) )
      result << QgsPalLayerSettings::TopLeft;
    else if ( cleaned == QLatin1String( "TSL" ) )
      result << QgsPalLayerSettings::TopSlightlyLeft;
    else if ( cleaned == QLatin1String( "T" ) )
      result << QgsPalLayerSettings::TopMiddle;
    else if ( cleaned == QLatin1String( "TSR" ) )
      result << QgsPalLayerSettings::TopSlightlyRight;
    else if ( cleaned == QLatin1String( "TR" ) )
      result << QgsPalLayerSettings::TopRight;
    else if ( cleaned == QLatin1String( "L" ) )
      result << QgsPalLayerSettings::MiddleLeft;
    else if ( cleaned == QLatin1String( "R" ) )
      result << QgsPalLayerSettings::MiddleRight;
    else if ( cleaned == QLatin1String( "BL" ) )
      result << QgsPalLayerSettings::BottomLeft;
    else if ( cleaned == QLatin1String( "BSL" ) )
      result << QgsPalLayerSettings::BottomSlightlyLeft;
    else if ( cleaned == QLatin1String( "B" ) )
      result << QgsPalLayerSettings::BottomMiddle;
    else if ( cleaned == QLatin1String( "BSR" ) )
      result << QgsPalLayerSettings::BottomSlightlyRight;
    else if ( cleaned == QLatin1String( "BR" ) )
      result << QgsPalLayerSettings::BottomRight;
  }
  return result;
}

QString QgsLabelingUtils::encodeLinePlacementFlags( QgsLabeling::LinePlacementFlags flags )
{
  QStringList parts;
  if ( flags & QgsLabeling::LinePlacementFlag::OnLine )
    parts << QStringLiteral( "OL" );
  if ( flags & QgsLabeling::LinePlacementFlag::AboveLine )
    parts << QStringLiteral( "AL" );
  if ( flags & QgsLabeling::LinePlacementFlag::BelowLine )
    parts << QStringLiteral( "BL" );
  if ( !( flags & QgsLabeling::LinePlacementFlag::MapOrientation ) )
    parts << QStringLiteral( "LO" );
  return parts.join( ',' );
}

QgsLabeling::LinePlacementFlags QgsLabelingUtils::decodeLinePlacementFlags( const QString &string )
{
  QgsLabeling::LinePlacementFlags flags = QgsLabeling::LinePlacementFlags();
  const QStringList flagList = string.split( ',' );
  bool foundLineOrientationFlag = false;
  for ( const QString &flag : flagList )
  {
    QString cleaned = flag.trimmed().toUpper();
    if ( cleaned == QLatin1String( "OL" ) )
      flags |= QgsLabeling::LinePlacementFlag::OnLine;
    else if ( cleaned == QLatin1String( "AL" ) )
      flags |= QgsLabeling::LinePlacementFlag::AboveLine;
    else if ( cleaned == QLatin1String( "BL" ) )
      flags |= QgsLabeling::LinePlacementFlag::BelowLine;
    else if ( cleaned == QLatin1String( "LO" ) )
      foundLineOrientationFlag = true;
  }
  if ( !foundLineOrientationFlag )
    flags |= QgsLabeling::LinePlacementFlag::MapOrientation;
  return flags;
}
