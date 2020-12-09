/*
** Surge Synthesizer is Free and Open Source Software
**
** Surge is made available under the Gnu General Public License, v3.0
** https://www.gnu.org/licenses/gpl-3.0.en.html
**
** Copyright 2004-2020 by various individuals as described by the Git transaction log
**
** All source at: https://github.com/surge-synthesizer/surge.git
**
** Surge was a commercial product from 2004-2018, with Copyright and ownership
** in that period held by Claes Johanson at Vember Audio. Claes made Surge
** open source in September 2018.
*/

#include "MSEGEditor.h"
#include "MSEGModulationHelper.h"
#include "guihelpers.h"
#include "DebugHelpers.h"
#include "SkinColors.h"
#include "basic_dsp.h" // for limit_range
#include "CNumberField.h"
#include "CScalableBitmap.h"
#include "SurgeBitmaps.h"
#include "CHSwitch2.h"
#include "CSwitchControl.h"
#include "SurgeGUIEditor.h"
#include "RuntimeFont.h"
#include "CursorControlGuard.h"
#include "guihelpers.h"

using namespace VSTGUI;

struct MSEGCanvas;

struct MSEGControlRegion : public CViewContainer, public Surge::UI::SkinConsumingComponent, public VSTGUI::IControlListener {
   MSEGControlRegion(const CRect &size, MSEGCanvas *c, SurgeStorage *storage, LFOStorage *lfos, MSEGStorage *ms, MSEGEditor::State *eds, Surge::UI::Skin::ptr_t skin, std::shared_ptr<SurgeBitmaps> b ): CViewContainer( size ) {
      setSkin( skin, b );
      this->ms = ms;
      this->eds = eds;
      this->lfodata = lfos;
      this->canvas = c;
      this->storage = storage;
      setBackgroundColor(skin->getColor(Colors::MSEGEditor::Panel));
      rebuild();
   };

   enum {
      tag_segment_nodeedit_mode = metaparam_offset + 1000, // Just to push outside any ID range
      tag_segment_movement_mode,
      tag_vertical_snap,
      tag_vertical_value,
      tag_horizontal_snap,
      tag_horizontal_value,
      tag_loop_mode,
      tag_edit_mode,
   } tags;
   
   void rebuild();
   virtual void valueChanged( CControl *p ) override;
   virtual int32_t controlModifierClicked (CControl* pControl, CButtonState button) override;

   virtual void draw( CDrawContext *dc) override {
      auto r = getViewSize();
      dc->setFillColor(skin->getColor(Colors::MSEGEditor::Panel));
      dc->drawRect( r, kDrawFilled );
   }
   
   MSEGStorage *ms = nullptr;
   MSEGEditor::State *eds = nullptr;
   MSEGCanvas *canvas = nullptr;
   LFOStorage *lfodata = nullptr;
   SurgeStorage *storage = nullptr;
};



struct MSEGCanvas : public CControl, public Surge::UI::SkinConsumingComponent, public Surge::UI::CursorControlAdapter<MSEGCanvas> {
   MSEGCanvas(const CRect &size, LFOStorage *lfodata, MSEGStorage *ms, MSEGEditor::State *eds, Surge::UI::Skin::ptr_t skin, std::shared_ptr<SurgeBitmaps> b ):
         CControl( size ), Surge::UI::CursorControlAdapter<MSEGCanvas>(nullptr) {
      setSkin( skin, b );
      this->ms = ms;
      this->eds = eds;
      this->lfodata = lfodata;
      this->axisWidth = std::max( 1.f, ms->totalDuration );
      Surge::MSEG::rebuildCache( ms );
      handleBmp = b->getBitmap( IDB_MSEG_NODES );
      timeEditMode = (MSEGCanvas::TimeEdit)eds->timeEditMode;
      setMouseableArea(getViewSize());
   };

   /*
   ** We make a list of hotzones when we draw so we don't have to recalculate the 
   ** mouse locations in drag and so on events
   */
   struct hotzone {
      CRect rect;
      CRect drawRect;
      bool useDrawRect = false;
      int associatedSegment;
      bool specialEndpoint = false; // For pan and zoom we need to treat the endpoint of the last segment specially
      bool active = false;
      bool dragging = false;
      
      // More coming soon
      enum Type
      {
         MOUSABLE_NODE,
         INACTIVE_NODE, // To keep the array the same size add dummies when you supress controls
         LOOPMARKER
      } type;

      enum ZoneSubType
      {
         SEGMENT_ENDPOINT,
         SEGMENT_CONTROL,
         LOOP_START,
         LOOP_END
      } zoneSubType = SEGMENT_CONTROL;

      enum SegmentControlDirection
      {
         VERTICAL_ONLY = 1,
         HORIZONTAL_ONLY,
         BOTH_DIRECTIONS
      } segmentDirection = VERTICAL_ONLY;
      std::function<void(float,float, const CPoint &)> onDrag;
   };
   std::vector<hotzone> hotzones;

   static constexpr int drawInsertX = 10, drawInsertY = 10, axisSpaceX = 18, axisSpaceY = 8;

   inline float drawDuration() {
      /*
      if( ms->n_activeSegments == 0 )
         return 1.0;
      return std::max( 1.0f, ms->totalDuration );
       */
      return axisWidth;
   }
   
   inline CRect getDrawArea() {
      auto vs = getViewSize();
      auto drawArea = vs.inset( drawInsertX, drawInsertY );
      drawArea.bottom -= axisSpaceY;
      drawArea.left += axisSpaceX;
      drawArea.top += 5;
      return drawArea;
   }

   inline CRect getHAxisArea() {
      auto vs = getViewSize();
      auto drawArea = vs.inset( drawInsertX, drawInsertY );
      drawArea.top = drawArea.bottom - axisSpaceY;
      drawArea.left += axisSpaceX;
      return drawArea;
   }
   inline CRect getVAxisArea() {
      auto vs = getViewSize();
      auto drawArea = vs.inset( drawInsertX, drawInsertY );
      drawArea.right = drawArea.left + axisSpaceX;
      drawArea.bottom -= axisSpaceY;
      return drawArea;
   }

   std::function<float(float)> valToPx() {
      auto drawArea = getDrawArea();
      float vscale = drawArea.getHeight();
      return [vscale, drawArea](float vp) {
                auto v = 1 - ( vp + 1 ) * 0.5;
                return v * vscale + drawArea.top;
             };
   }
   std::function<float(float)> pxToVal() {
      auto drawArea = getDrawArea();
      float vscale = drawArea.getHeight();
      return [vscale, drawArea](float vx) {
                auto v = ( vx - drawArea.top ) / vscale;
                auto vp = ( 1 - v ) * 2 - 1;
                return vp;
             };
   }
   std::function<float(float)> timeToPx() {
      auto drawArea = getDrawArea();
      float maxt = drawDuration();
      float tscale = 1.f * drawArea.getWidth() / maxt;
      return [tscale, drawArea, this](float t) { return ( t - axisStart ) * tscale + drawArea.left; };
   }
   std::function<float(float)> pxToTime() { // INVESTIGATE
      auto drawArea = getDrawArea();
      float maxt = drawDuration();
      float tscale = 1.f * drawArea.getWidth() / maxt;

      // So px = t * tscale + drawarea;
      // So t = ( px - drawarea ) / tscale;
      return [tscale, drawArea, this ](float px) {
                return (px - drawArea.left) / tscale + axisStart;
             };
   }

   void offsetValue( float &v, float d ) {
      v = limit_range( v + d, -1.f, 1.f );
   }
   void adjustValue( int idx, bool cpvNotV0, float d, float snapResolution )
   {
      if( cpvNotV0 )
      {
         offsetValue( ms->segments[idx].cpv, d );
      }
      else
      {
         offsetValue( ms->segments[idx].dragv0, d );
         if( snapResolution <= 0 )
            ms->segments[idx].v0 = ms->segments[idx].dragv0;
         else
         {
            float q = ms->segments[idx].dragv0 + 1;
            float pos = round( q / snapResolution ) * snapResolution;
            float adj = pos - 1.0;
            ms->segments[idx].v0 = limit_range(adj, -1.f, 1.f );
         }
      }
   }

   /*
    * This little struct acts as a SmapGuard so that shift drags can reset snap and
    * unreset snap
    */
   struct SnapGuard {
      SnapGuard( MSEGEditor::State *eds, MSEGCanvas *c ) : eds(eds), c(c) {
         hSnapO = eds->hSnap;
         vSnapO = eds->vSnap;
         c->invalid();
      }
      ~SnapGuard() {
         eds->hSnap = hSnapO;
         eds->vSnap = vSnapO;
         c->invalid();
      }
      MSEGEditor::State *eds;
      MSEGCanvas *c;
      float hSnapO, vSnapO;
   };
   std::shared_ptr<SnapGuard> snapGuard;


   enum TimeEdit
   {
      SINGLE,   // movement bound between two neighboring nodes
      SHIFT,    // shifts all following nodes along, relatively
      DRAW,     // only change amplitude of nodes as the cursor passes along the timeline
   } timeEditMode = SINGLE;

   void recalcHotZones( const CPoint &where ) {
      hotzones.clear();

      auto drawArea = getDrawArea();
      
      int handleRadius = 6;

      float maxt = drawDuration();
      float tscale = 1.f * drawArea.getWidth() / maxt;
      float vscale = drawArea.getHeight();
      auto valpx = valToPx();
      auto tpx = timeToPx();

      // Put in the loop marker boxes
      if( ms->loopMode != MSEGStorage::LoopMode::ONESHOT ){
         int ls = (ms->loop_start >= 0 ? ms->loop_start : 0);
         int le = (ms->loop_end >= 0 ? ms->loop_end : ms->n_activeSegments - 1);
         float pxs = tpx(ms->segmentStart[ls]);
         float pxe = tpx(ms->segmentEnd[le]);
         auto hs = hotzone();
         hs.type = hotzone::Type::LOOPMARKER;
         hs.segmentDirection = hotzone::HORIZONTAL_ONLY;
         hs.associatedSegment = -1;

         auto he = hs;

         auto haxisArea = getHAxisArea();

         if( this->ms->editMode != MSEGStorage::LFO )
         {
            int lmSize = 10;
            hs.rect = VSTGUI::CRect(CPoint(pxs, haxisArea.top + 1), CPoint(lmSize, lmSize));
            hs.zoneSubType = hotzone::LOOP_START;

            he.rect =
                VSTGUI::CRect(CPoint(pxe - lmSize, haxisArea.top + 1), CPoint(lmSize, lmSize));
            he.zoneSubType = hotzone::LOOP_END;

            hotzones.push_back(hs);
            hotzones.push_back(he);
         }
      }

      for( int i=0; i<ms->n_activeSegments; ++i )
      {
         auto t0 = tpx(ms->segmentStart[i]);
         auto t1 = tpx(ms->segmentEnd[i]);

         auto segrec = CRect(t0, drawArea.top, t1, drawArea.bottom);

         // Now add the mousable zones
         auto& s = ms->segments[i];
         auto rectForPoint = [&](float t, float v, hotzone::ZoneSubType mt,
                                 std::function<void(float, float, const CPoint&)> onDrag) {
            auto h = hotzone();
            h.rect = CRect(t - handleRadius, valpx(v) - handleRadius, t + handleRadius,
                           valpx(v) + handleRadius);
            h.type = hotzone::Type::MOUSABLE_NODE;
            if (h.rect.pointInside(where))
               h.active = true;
            h.onDrag = onDrag;
            h.associatedSegment = i;
            h.zoneSubType = mt;
            hotzones.push_back(h);
         };

         auto timeConstraint = [&](int prior, float dx) {
            switch (this->timeEditMode)
            {
            case DRAW:
               break;
            case SHIFT:
               Surge::MSEG::adjustDurationShiftingSubsequent(this->ms, prior, dx, eds->hSnap);
               break;
            case SINGLE:
               Surge::MSEG::adjustDurationConstantTotalDuration(this->ms, prior, dx, eds->hSnap);
               break;
            }
         };

         auto unipolarFactor = 1 + lfodata->unipolar.val.b;

         // We get a mousable point at the start of the line
         rectForPoint(
             t0, s.v0, hotzone::SEGMENT_ENDPOINT,
             [i, this, vscale, tscale, timeConstraint, unipolarFactor](float dx, float dy, const CPoint& where) {
                adjustValue(i, false, -2 * dy / vscale, eds->vSnap * unipolarFactor);

                if (i != 0)
                {
                   timeConstraint(i - 1, dx / tscale);
                }
             });

         // Control point editor
         if (ms->segments[i].duration > 0.01 && ms->segments[i].type != MSEGStorage::segment::HOLD )
         {
            /*
             * Drop in a control point. But where and moving how?
             *
             * Here's some good defaults
             */
            bool verticalMotion = true;
            bool horizontalMotion = false;
            bool verticalScaleByValues = false;

            switch (ms->segments[i].type)
            {
            // Ones where we scan the entire square
            case MSEGStorage::segment::QUAD_BEZIER:
            case MSEGStorage::segment::BROWNIAN:
               horizontalMotion = true;
               break;
            // Ones where we stay within the range
            case MSEGStorage::segment::LINEAR:
               verticalScaleByValues = true;
               break;
            default:
               break;
            }

            // Follow the mouse with scaling of course

            float vLocation = ms->segments[i].cpv;
            if( verticalScaleByValues )
               vLocation = 0.5 * (vLocation + 1) * ( ms->segments[i].nv1 - ms->segments[i].v0 ) + ms->segments[i].v0;


            // switch is here in case some other curves need the fixed control point in other places horizontally
            float fixtLocation;
            switch (ms->segments[i].type)
            {
            default:
               fixtLocation = 0.5;
               break;
            }

            float tLocation = fixtLocation * ms->segments[i].duration + ms->segmentStart[i];

            if( horizontalMotion )
               tLocation = ms->segments[i].cpduration * ms->segments[i].duration + ms->segmentStart[i];

            // std::cout << _D(i) << _D(ms->segments[i].type) << _D(ms->segments[i].cpv) << _D(ms->segments[i].cpduration) << _D(verticalMotion) << _D(verticalScaleByValues) << _D(horizontalMotion) << _D(vLocation ) << _D(tLocation) << _D(ms->segmentStart[i] ) << std::endl;

            float t = tpx( tLocation );
            float v = valpx( vLocation );
            auto h = hotzone();
            h.rect = CRect(t - handleRadius, v - handleRadius, t + handleRadius,
                           v + handleRadius);
            h.type = hotzone::Type::MOUSABLE_NODE;
            if (h.rect.pointInside(where))
               h.active = true;


            h.useDrawRect = false;
            if( ( verticalMotion && ! horizontalMotion &&
                (ms->segments[i].type != MSEGStorage::segment::LINEAR &&
                 ms->segments[i].type != MSEGStorage::segment::BUMP)) ||
                ms->segments[i].type == MSEGStorage::segment::BROWNIAN )
            {
               float t = tpx( 0.5 * ms->segments[i].duration + ms->segmentStart[i] );
               float v = valpx( 0.5 * (ms->segments[i].nv1 + ms->segments[i].v0 ) );
               h.drawRect = CRect(t - handleRadius, v - handleRadius, t + handleRadius,
                                  v + handleRadius);
               h.rect = h.drawRect;
               h.useDrawRect = true;
            }

            float segdt = ms->segmentEnd[i] - ms->segmentStart[i];
            float segdx = ms->segments[i].nv1 - ms->segments[i].v0;

            h.onDrag = [this, i, tscale, vscale, verticalMotion, horizontalMotion, verticalScaleByValues, segdt , segdx](float dx, float dy, const CPoint &where) {
               if( verticalMotion )
               {
                  float dv = 0;
                  if( verticalScaleByValues)
                     dv = -2 * dy / vscale / (0.5 * segdx );
                  else
                     dv = -2 * dy / vscale;

                  /*
                   * OK we have some special case edge scalings for sensitive types.
                   * If this ends up applying to more than linear then I would be surprised
                   * but write a switch anyway :)
                   */
                  switch( ms->segments[i].type )
                  {
                  case MSEGStorage::segment::LINEAR:
                  case MSEGStorage::segment::SCURVE:
                  {
                     // Slowdown parameters. Basically slow down mouse -> delta linearly near edge
                     float slowdownInTheLast = 0.15;
                     float slowdownAtMostTo = 0.015;
                     float adj = 1;

                     if( ms->segments[i].cpv > (1.0-slowdownInTheLast) )
                     {
                        auto lin = (ms->segments[i].cpv - slowdownInTheLast)/(1.0-slowdownInTheLast);
                        adj = ( 1.0 - (1.0-slowdownAtMostTo) * lin);
                     }
                     else if( ms->segments[i].cpv < (-1.0 + slowdownInTheLast ))
                     {
                        auto lin = (ms->segments[i].cpv + slowdownInTheLast)/(-1.0+slowdownInTheLast);
                        adj = ( 1.0 - (1.0-slowdownAtMostTo) * lin);
                     }
                     dv *= adj;
                  }
                  default:
                     break;
                  }

                  ms->segments[i].cpv += dv;
               }
               if( horizontalMotion )
                  ms->segments[i].cpduration += dx / tscale / segdt;
               Surge::MSEG::constrainControlPointAt(ms, i);
               modelChanged(i);
            };
            h.associatedSegment = i;
            h.zoneSubType = hotzone::SEGMENT_CONTROL;
            if( verticalMotion && horizontalMotion )
               h.segmentDirection = hotzone::BOTH_DIRECTIONS;
            else if( horizontalMotion )
               h.segmentDirection = hotzone::HORIZONTAL_ONLY;
            else
               h.segmentDirection = hotzone::VERTICAL_ONLY;

            hotzones.push_back(h);
         }
         else
         {
            hotzone h;
            h.type = hotzone::INACTIVE_NODE;
            hotzones.push_back(h);
         }

         // Specially we have to add an endpoint editor
         if( i == ms->n_activeSegments - 1 )
         {
            rectForPoint( tpx(ms->totalDuration), ms->segments[ms->n_activeSegments-1].nv1, /* which is [0].v0 in lock mode only */
                         hotzone::SEGMENT_ENDPOINT,
                          [this, vscale, tscale, unipolarFactor](float dx, float dy, const CPoint &where) {
                             if( ms->endpointMode == MSEGStorage::EndpointMode::FREE && ms->editMode != MSEGStorage::LFO )
                             {
                                float d = -2 * dy / vscale;
                                float snapResolution = eds->vSnap * unipolarFactor;
                                int idx = ms->n_activeSegments - 1;
                                offsetValue( ms->segments[idx].dragv1, d );
                                if( snapResolution <= 0 )
                                   ms->segments[idx].nv1 = ms->segments[idx].dragv1;
                                else
                                {
                                   float q = ms->segments[idx].dragv1+ 1;
                                   float pos = round( q / snapResolution ) * snapResolution;
                                   float adj = pos - 1.0;
                                   ms->segments[idx].nv1= limit_range(adj, -1.f, 1.f );
                                }
                             }
                             else
                             {
                                adjustValue(0, false, -2 * dy / vscale, eds->vSnap);
                             }
                             // We need to deal with the cpduration also
                             auto cpv = this->ms->segments[ms->n_activeSegments-1].cpduration / this->ms->segments[ms->n_activeSegments-1].duration;

                             // Don't allow endpoint time adjust in LFO mode
                             if( this->ms->editMode == MSEGStorage::ENVELOPE )
                             {
                                if (!this->getViewSize().pointInside(where))
                                {
                                   auto howFar = where.x - this->getViewSize().right;
                                   if (howFar > 0)
                                      dx *= howFar * 0.1; // this is really just a speedup as our axes shrinks. Just a fudge
                                }

                                Surge::MSEG::adjustDurationShiftingSubsequent(
                                    ms, ms->n_activeSegments - 1, dx / tscale, eds->hSnap);
                             }
                          } );
            hotzones.back().specialEndpoint = true;
         }
      }
                
   }

   // grid thinning
   const int gridMaxHSteps = 20, gridMaxVSteps = 10;

   inline void drawAxis( CDrawContext *dc ) {

      auto primaryFont = new VSTGUI::CFontDesc("Lato", 9, kBoldFace);
      auto secondaryFont = new VSTGUI::CFontDesc("Lato", 7);

      auto uni = lfodata->unipolar.val.b;
      auto haxisArea = getHAxisArea();
      float maxt = drawDuration();
      auto tpx = timeToPx();

      dc->setFrameColor(skin->getColor(Colors::MSEGEditor::Axis::Line));
      dc->drawLine( haxisArea.getTopLeft(), haxisArea.getTopRight() );

      updateHTicks();
      for( auto hp : hTicks )
      {
         float t = hp.first;
         auto c = hp.second;
         float px = tpx(t);
         float off = haxisArea.getHeight() / 2;
         int xoff = 0;

         if( c & TickDrawStyle::kHighlight )
         {
            off = 0;
            dc->setFrameColor(skin->getColor(Colors::MSEGEditor::Axis::Line));
            dc->setLineWidth(1.5);
         }
         else
         {
            dc->setFrameColor(skin->getColor(Colors::MSEGEditor::Grid::SecondaryVertical));
            dc->setLineWidth(1.f);
         }

         dc->drawLine( CPoint( px, haxisArea.top), CPoint( px, haxisArea.bottom - off ) );

         char txt[16];

         if ( ! ( c & TickDrawStyle::kNoLabel ) )
         {
            if (fmod(t, 1.f) == 0.f)
            {
               dc->setFontColor(skin->getColor(Colors::MSEGEditor::Axis::Text));
               dc->setFont(primaryFont);
               xoff = 0;
               snprintf( txt, 16, "%d", int(t) );
            }
            else
            {
               dc->setFontColor(skin->getColor(Colors::MSEGEditor::Axis::SecondaryText));
               dc->setFont(secondaryFont);
               xoff = -7;
               snprintf( txt, 16, "%5.2f", t );
            }
            dc->drawString( txt, CRect( CPoint( px + xoff, haxisArea.top + 5), CPoint( 15, 10 )));
         }
      }

      // draw loop markers
      if (0 && ms->loopMode > 1)
      {
         int ls = ( ms->loop_start >= 0 ? ms->loop_start : 0 );
         int le = ( ms->loop_end >= 0 ? ms->loop_end : ms->n_activeSegments - 1 );
         float pxs = tpx( ms->segmentStart[ls] );
         float pxe = tpx(ms->segmentEnd[le]);
      
         auto r = VSTGUI::CRect( CPoint( pxs, haxisArea.top + 1), CPoint( 10, 10 ));
         dc->setFillColor(CColor(0, 255, 0, 128));
         dc->drawRect( r, kDrawFilled );

         r = VSTGUI::CRect( CPoint( pxe - 8, haxisArea.top + 1), CPoint( 10, 10 ));
         dc->setFillColor(CColor(255, 0, 0, 128));
         dc->drawRect( r, kDrawFilled );
      }

      // vertical axis
      auto vaxisArea = getVAxisArea();
      auto valpx = valToPx();

      dc->setLineWidth( 1.5 );

      dc->setFrameColor(skin->getColor(Colors::MSEGEditor::Axis::Line));
      dc->drawLine( vaxisArea.getTopRight(), vaxisArea.getBottomRight() );

      dc->setFont(primaryFont);
      dc->setFontColor(skin->getColor(Colors::MSEGEditor::Axis::Text));

      updateVTicks();

      for( auto vp : vTicks )
      {
         float p = valpx(std::get<0>(vp));

         float off = vaxisArea.getWidth() / 2;

         if (std::get<2>(vp))
            off = 0;

         if( off == 0 )
            dc->setFrameColor(skin->getColor(Colors::MSEGEditor::Axis::Line));
         else
            dc->setFrameColor(skin->getColor(Colors::MSEGEditor::Grid::SecondaryHorizontal));

         dc->drawLine( CPoint( vaxisArea.left + off, p ), CPoint( vaxisArea.right, p ) );

         if( off == 0 )
         {
            char txt[16];
            auto value = std::get<1>(vp);

            if (value == 0.f && std::signbit(value))
                value = -value;

            snprintf(txt, 16, "%5.1f", value);
            dc->drawString(txt, CRect( CPoint( vaxisArea.left, p - 10 ), CPoint( 10, 10 )));
         }
      }
   }

   enum TickDrawStyle {
      kNoLabel = 1<<0,
      kHighlight = 1<<1
   };

   std::vector<std::pair<float,int>> hTicks;
   float hTicksAsOf[3] = {-1,-1,-1};
   void updateHTicks()
   {
      if( hTicksAsOf[0] == eds->hSnapDefault &&
          hTicksAsOf[1] == axisStart &&
          hTicksAsOf[2] == axisWidth
          )
         return;

      hTicksAsOf[0] = eds->hSnapDefault;
      hTicksAsOf[1] = axisStart;
      hTicksAsOf[2] = axisWidth;

      hTicks.clear();

      float dStep = eds->hSnapDefault;
      /*
       * OK two cases - step makes a squillion white lines, or step makes too few lines. Both of
       * these depend on this ratio
       */
      float widthByStep = axisWidth / dStep;

      if( widthByStep < 4 )
      {
         while( widthByStep < 4 )
         {
            dStep /= 2;
            widthByStep = axisWidth / dStep;
         }
      }
      else if( widthByStep > 20 )
      {
         while( widthByStep > 20 )
         {
            dStep *= 2;
            widthByStep = axisWidth / dStep;
         }
      }

      // OK so what's our zero point.
      int startPoint = ceil(axisStart/dStep);
      int endPoint = ceil( (axisStart + axisWidth ) / dStep );

      for( int i=startPoint; i<=endPoint; ++i )
      {
         float f = i * dStep;
         bool isInt = fabs( f - round( f ) ) < 1e-4;

         hTicks.push_back( std::make_pair( f, isInt ? kHighlight : 0 ) );
      }

   }

   std::vector<std::tuple<float,float,bool>> vTicks; // position, display, is highlight
   float vTickAsOf = -1;
   void updateVTicks()
   {
      auto uni = lfodata->unipolar.val.b;

      if( eds->vSnapDefault != vTickAsOf )
      {
         vTicks.clear();
         float dStep = eds->vSnapDefault;
         while (dStep < 1.0 / (gridMaxVSteps * (1 + uni)))
            dStep *= 2;

         int steps = ceil(1.0 / dStep);
         int start, end;
         if (uni)
         {
            start = 0;
            end = steps + 1;
         }
         else
         {
            start = -steps - 1;
            end = steps + 1;
         }

         for (int vi = start; vi < end; vi++)
         {
            float val = vi * dStep;
            auto doDraw = true;

            if (val > 1)
            {
               val = 1.0;
               if (vi != end - 1)
                  doDraw = false;
            }
            if (val < -1)
            {
               val = -1.0;
               if (vi != start)
                  doDraw = false;
            }
            if (uni && val < 0)
            {
               val = 0;
               if (vi != start)
                  doDraw = false;
            }
            if( doDraw )
            {
               // Little secret: Unipolar is just a relabeled Bipolars
               float pval = val;
               if( uni )
               {
                  pval = val * 2 - 1;
               }
               vTicks.push_back(std::make_tuple(pval, val, vi == 0 || vi == start || vi == end - 1));
            }
         }
      }
   }

   virtual void draw( CDrawContext *dc) override {
      auto uni = lfodata->unipolar.val.b;
      auto vs = getViewSize();

      if (hotzones.empty())
         recalcHotZones( CPoint( vs.left, vs.top ) );

      dc->setFillColor(skin->getColor(Colors::MSEGEditor::Background));
      dc->drawRect( vs, kDrawFilled );
      
      // we want to draw the background rectangle always filling the area without smearing
      // so draw the rect first then set AA drawing mode
      dc->setDrawMode(kAntiAliasing);
      
      auto valpx = valToPx();
      auto tpx = timeToPx();
      auto pxt = pxToTime();

      auto drawArea = getDrawArea();
      float maxt = drawDuration();

      Surge::MSEG::EvaluatorState es, esdf;
      es.seed( 8675309 ); // This is different from the number in LFOMS::assign in draw mode on purpose
      esdf.seed( 8675309 );

      CGraphicsPath *path = dc->createGraphicsPath();
      CGraphicsPath *highlightPath = dc->createGraphicsPath();
      CGraphicsPath *defpath = dc->createGraphicsPath();
      CGraphicsPath *fillpath = dc->createGraphicsPath();

      float pathScale = 1.0;

      auto xdisp = drawArea;
      float yOff = drawArea.top;

      auto beginP = [yOff, pathScale](CGraphicsPath *p, CCoord x, CCoord y )
      {
         p->beginSubpath(pathScale * x,pathScale * (y-yOff));
      };

      auto addP = [yOff, pathScale](CGraphicsPath *p, CCoord x, CCoord y )
      {
        p->addLine(pathScale * x,pathScale * (y-yOff));
      };

      bool hlpathUsed = false;

      float pathFirstY, pathLastX, pathLastY, pathLastDef;

      bool drawnLast = false; // this slightly odd construct means we always draw beyond the last point
      int priorEval = 0;
      for( int q=0; q<drawArea.getWidth(); ++q )
      {
         float up = pxt( q + drawArea.left );
         int i = q;
         if( ! drawnLast )
         {
            float iup = (int)up;
            float fup = up - iup;
            float v = Surge::MSEG::valueAt( iup, fup, 0, ms, &es, true );
            float vdef = Surge::MSEG::valueAt( iup, fup, lfodata->deform.val.f, ms, &esdf, true );

            v = valpx( v );
            vdef = valpx( vdef );
            // Brownian doesn't deform and the second display is confusing since it is indepdently random
            if( es.lastEval >= 0 && es.lastEval <= ms->n_activeSegments - 1 && ms->segments[es.lastEval].type == MSEGStorage::segment::Type::BROWNIAN )
               vdef = v;

            int compareWith = es.lastEval;
            if( up > ms->totalDuration )
               compareWith = ms->n_activeSegments - 1;

            if( compareWith != priorEval )
            {
               // OK so make sure that priorEval nv1 is in there
               addP( path, i, valpx(ms->segments[priorEval].nv1));
               for( int ns=priorEval + 1; ns <= compareWith; ns++ )
               {
                  // Special case - hold draws endpoint
                  if (ns > 0 && ms->segments[ns-1].type == MSEGStorage::segment::HOLD )
                     addP( path, i, valpx( ms->segments[ns-1].v0));
                  addP( path, i, valpx(ms->segments[ns].v0));
               }
               priorEval = es.lastEval;
            }

            if( es.lastEval == hoveredSegment )
            {
               if( !hlpathUsed )
               {
                  beginP(highlightPath, i, v);
                  hlpathUsed = true;
               }
               else
               {
                  addP( highlightPath, i, v );
               }
            }

            if( i == 0 )
            {
               beginP( path, i, v  );
               beginP( defpath, i, vdef );
               beginP( fillpath, i, v );
               pathFirstY = v;
            }
            else
            {
               addP( path, i, v );
               addP( defpath, i, vdef );
               addP( fillpath, i, v );
            }
            pathLastX = i; pathLastY = v; pathLastDef = vdef;
         }
         drawnLast = up > ms->totalDuration;
      }

      int uniLimit = 0;
#if LINUX
      // Linux has a bug that it ignores the zoom
      auto tfpath = CGraphicsTransform()
                        .scale(1.0/pathScale, 1.0/pathScale)
                        .translate(getFrame()->getZoom() * drawArea.left, getFrame()->getZoom() * drawArea.top);
#else
      auto tfpath = CGraphicsTransform()
                        .scale(1.0/pathScale, 1.0/pathScale)
                        .translate(drawArea.left, drawArea.top);
#endif

      VSTGUI::CGradient::ColorStopMap csm;
      VSTGUI::CGradient* cg = VSTGUI::CGradient::create(csm);
      
      cg->addColorStop(0, skin->getColor(Colors::MSEGEditor::GradientFill::StartColor));
      if (uni)
      {
         uniLimit = -1;
         cg->addColorStop(1, skin->getColor(Colors::MSEGEditor::GradientFill::EndColor));
      }
      else
      {
         cg->addColorStop(0.5, skin->getColor(Colors::MSEGEditor::GradientFill::EndColor));
         cg->addColorStop(1, skin->getColor(Colors::MSEGEditor::GradientFill::StartColor));
      }

      addP( fillpath, pathLastX, valpx(uniLimit));
      addP( fillpath, uniLimit, valpx(uniLimit));
      addP( fillpath, uniLimit, pathFirstY);

      // Make sure to restore this
#if LINIX
      Surge::UI::NonIntegralAntiAliasGuard naig(dc);
#endif

      dc->fillLinearGradient(fillpath, *cg, CPoint(0, 0), CPoint(0, valpx(-1)), false, &tfpath);
      fillpath->forget();
      cg->forget();
      
      dc->setLineWidth(1);

      // draw vertical grid
      auto primaryGridColor = skin->getColor(Colors::MSEGEditor::Grid::Primary);
      auto secondaryHGridColor = skin->getColor(Colors::MSEGEditor::Grid::SecondaryHorizontal);
      auto secondaryVGridColor = skin->getColor(Colors::MSEGEditor::Grid::SecondaryVertical);

      updateHTicks();
      for( auto hp : hTicks )
      {
         auto t = hp.first;
         auto c = hp.second;
         auto px = tpx( t );

         if (c & TickDrawStyle::kHighlight)
         {
            dc->setFrameColor( primaryGridColor );
            dc->setLineWidth(1.5);
         }
         else
         {
            dc->setFrameColor(secondaryVGridColor);
            dc->setLineWidth(1);
         }

         if (t > 0.1)
            dc->drawLine( CPoint( px, drawArea.top ), CPoint( px, drawArea.bottom ) );
      }

      dc->setLineWidth(1);

      updateVTicks();

      for( auto vp : vTicks )
      {
         float val = std::get<0>( vp );
         float v = valpx(val);

         if ( std::get<2>(vp) )
         {
            dc->setFrameColor(primaryGridColor);
            dc->setLineStyle(kLineSolid);
         }
         else
         {
            dc->setFrameColor(secondaryHGridColor);
            CCoord dashes[] = {2, 5};
            dc->setLineStyle(CLineStyle(CLineStyle::kLineCapButt, CLineStyle::kLineJoinMiter, 0, 2, dashes));
         }

         dc->drawLine(CPoint(drawArea.left, v), CPoint(drawArea.right, v));
      }

      // Draw the axes here after the gradient fill and gridlines
      drawAxis(dc);
      dc->setLineStyle(CLineStyle(VSTGUI::CLineStyle::kLineCapButt, VSTGUI::CLineStyle::kLineJoinBevel));

      // draw segment curve
      dc->setLineWidth(0.75 * pathScale);
      dc->setFrameColor(skin->getColor( Colors::MSEGEditor::DeformCurve));
      dc->drawGraphicsPath(defpath, VSTGUI::CDrawContext::PathDrawMode::kPathStroked, &tfpath);

      dc->setLineWidth(1.0 * pathScale);
      dc->setFrameColor(skin->getColor(Colors::MSEGEditor::Curve));
      dc->drawGraphicsPath(path, VSTGUI::CDrawContext::PathDrawMode::kPathStroked, &tfpath);

      // hovered segment curve is slightly thicker
      dc->setLineWidth(1.5 * pathScale);

      if( hlpathUsed )
      {
         dc->setFrameColor(skin->getColor(Colors::MSEGEditor::CurveHighlight));
         dc->drawGraphicsPath(highlightPath, VSTGUI::CDrawContext::PathDrawMode::kPathStroked, &tfpath);
      }

      path->forget();
      defpath->forget();
      highlightPath->forget();

      if( ! inDrag )
         getFrame()->setCursor(kCursorDefault);

      for( const auto &h : hotzones )
      {
         if( h.type == hotzone::LOOPMARKER )
         {
            // OK so we draw |< or >| depending
            if( h.active )
            {
               dc->setFillColor(skin->getColor(Colors::MSEGEditor::Loop::Marker)); // not we just hover the outline
               dc->setFrameColor(skin->getColor(Colors::MSEGEditor::Loop::MarkerHover));
            }
            else
            {
               dc->setFillColor(skin->getColor(Colors::MSEGEditor::Loop::Marker));
               dc->setFrameColor(skin->getColor(Colors::MSEGEditor::Loop::Marker));
            }
            dc->setLineWidth(1);
            dc->setLineStyle(kLineSolid);
            if( h.zoneSubType == hotzone::LOOP_START )
            {
               // line on left side arrow pointing over
               dc->drawLine(h.rect.getTopLeft(), h.rect.getBottomLeft());
               CDrawContext::PointList l;
               l.push_back( h.rect.getTopRight());
               l.push_back( h.rect.getBottomRight());
               l.push_back( CPoint( h.rect.left, ( h.rect.top + h.rect.bottom ) * 0.5 ));
               l.push_back( h.rect.getTopRight());
               dc->drawPolygon(l, kDrawFilledAndStroked);
            }
            else
            {
               dc->drawLine(h.rect.getTopRight(), h.rect.getBottomRight());
               CDrawContext::PointList l;
               l.push_back( h.rect.getTopLeft());
               l.push_back( h.rect.getBottomLeft());
               l.push_back( CPoint( h.rect.right, ( h.rect.top + h.rect.bottom ) * 0.5 ));
               l.push_back( h.rect.getTopLeft() );
               dc->drawPolygon(l, kDrawFilledAndStroked);

            }
         }

         if( h.type == hotzone::MOUSABLE_NODE )
         {
            int sz = 12;
            int offx = 0;

            int offy = 0;

            if( h.active )
               offy = 1;

            if( h.dragging )
               offy = 2;

            if( h.zoneSubType == hotzone::SEGMENT_CONTROL )
               offx = 1;

            if( h.active || h.dragging )
            {

               if (h.zoneSubType == hotzone::SEGMENT_CONTROL)
               {
                  auto nextCursor = VSTGUI::kCursorDefault;
                  if (h.active || h.dragging)
                  {
                     switch (h.segmentDirection)
                     {
                     case hotzone::VERTICAL_ONLY:
                        nextCursor = kCursorVSize;
                        break;
                     case hotzone::HORIZONTAL_ONLY:
                        nextCursor = kCursorHSize;
                        break;
                     case hotzone::BOTH_DIRECTIONS:
                        nextCursor = kCursorSizeAll;
                        break;
                     }

                     getFrame()->setCursor(nextCursor);
                  }
               }
               if (h.zoneSubType == hotzone::SEGMENT_ENDPOINT)
                  getFrame()->setCursor(kCursorSizeAll);
            }

            if( handleBmp )
            {
               auto r = h.rect;
               if( h.useDrawRect )
                  r = h.drawRect;
               int cx = r.getCenter().x;
               if( cx >= drawArea.left && cx <= drawArea.right )
                  handleBmp->draw( dc, r, CPoint( offx * sz, offy * sz ), 0xFF );
            }
         }
      }
   }

   CPoint mouseDownOrigin, cursorHideOrigin, lastPanZoomMousePos;
   bool inDrag = false;
   bool inDrawDrag = false;
   bool cursorHideEnqueued = false;
   virtual CMouseEventResult onMouseDown(CPoint &where, const CButtonState &buttons ) override {
      if (buttons & kRButton)
      {
         auto da = getDrawArea();
         if( da.pointInside( where ) )
         {
            openPopup(where);
         }
         return kMouseDownEventHandledButDontNeedMovedOrUpEvents;
      }
      if (buttons & kDoubleClick)
      {
         auto ha = getHAxisArea();
         if( ha.pointInside( where ) )
         {
            zoomToFull();
            return kMouseDownEventHandledButDontNeedMovedOrUpEvents;
         }

         auto da = getDrawArea();

         if( da.pointInside( where ) )
         {
            auto tf = pxToTime();
            auto t = tf(where.x);
            auto pv = pxToVal();
            auto v = pv(where.y);

            // Check if I'm on a hotzoneo
            for (auto& h : hotzones)
            {
               if (h.rect.pointInside(where) && h.type == hotzone::MOUSABLE_NODE)
               {
                  switch (h.zoneSubType)
                  {
                  case hotzone::SEGMENT_ENDPOINT: {
                     Surge::MSEG::unsplitSegment(ms, t);
                     modelChanged();
                     return kMouseEventHandled;
                  }
                  case hotzone::SEGMENT_CONTROL: {
                     // Reset the controlpoint to duration half value middle
                     Surge::MSEG::resetControlPoint(ms, t);
                     modelChanged();
                     return kMouseEventHandled;
                  }
                  case hotzone::LOOP_START:
                  case hotzone::LOOP_END:
                     // FIXME : Implement this
                     break;
                  }
               }
            }

            if (t < ms->totalDuration)
            {
               Surge::MSEG::splitSegment(ms, t, v);
               modelChanged();
               return kMouseEventHandled;
            }
            else
            {
               Surge::MSEG::extendTo(ms, t, v);
               modelChanged();
               return kMouseEventHandled;
            }
         }
      }

      for( auto &s : ms->segments )
      {
         s.dragv0 = s.v0;
         s.dragv1 = s.nv1;
         s.dragcpv = s.cpv;
         s.dragDuration = s.duration;
      }

      if( timeEditMode == DRAW )
      {
         // Allow us to initiate drags on control points in draw mode
         bool amOverControl = false;
         for (auto& h : hotzones)
         {
            if (h.rect.pointInside(where) && h.type == hotzone::MOUSABLE_NODE &&
                h.zoneSubType == hotzone::SEGMENT_CONTROL)
            {
               amOverControl = true;
            }
         }
         if( ! amOverControl )
         {
            inDrawDrag = true;
            return kMouseEventHandled;
         }
      }

      mouseDownOrigin = where;
      lastPanZoomMousePos = where;
      inDrag = true;
      bool foundHZ = false;
      for( auto &h : hotzones )
      {
         if( h.rect.pointInside(where) && h.type == hotzone::MOUSABLE_NODE )
         {
            foundHZ = true;
            cursorHideEnqueued = true;
            cursorHideOrigin = where;
            h.active = true;
            h.dragging = true;
            invalid();

            /*
             * Activate temporary snap. Note this is also handled similarly in
             * onMouseMoved so if you change ctrl/alt here change it there too
             */
            bool c = buttons & kControl;
            bool a = buttons & kAlt;
            if( c || a  )
            {
               snapGuard = std::make_shared<SnapGuard>(eds, this);
               if( c ) eds->hSnap = eds->hSnapDefault;
               if( a ) eds->vSnap = eds->vSnapDefault;
            }
            break;
         }
      }

      return kMouseEventHandled;
   }

   virtual CMouseEventResult onMouseUp(CPoint &where, const CButtonState &buttons ) override {
      getFrame()->setCursor( kCursorDefault );
      inDrag = false;
      inDrawDrag = false;

      for( auto &h : hotzones )
      {
         if( h.dragging )
         {
            if( h.type == hotzone::MOUSABLE_NODE )
            {
               if( h.zoneSubType == hotzone::SEGMENT_ENDPOINT )
               {
                  setCursorLocation(h.rect.getCenter());
               }
               if( h.zoneSubType == hotzone::SEGMENT_CONTROL && !h.useDrawRect )
               {
                  setCursorLocation(h.rect.getCenter());
               }
            }

         }
         h.dragging = false;
      }
      snapGuard = nullptr;
      endCursorHide();
      cursorHideEnqueued = false;

      return kMouseEventHandled;
   }
   virtual bool magnify(CPoint& where, float amount) override
   {
      zoom(where, amount, 0);
      return true;
   }

   virtual CMouseEventResult onMouseMoved(CPoint &where, const CButtonState &buttons ) override {
      auto tf = pxToTime( );
      auto t = tf( where.x );
      auto da = getDrawArea();

      if( da.pointInside( where ) )
      {
         auto ohs = hoveredSegment;
         if (t < 0)
         {
            hoveredSegment = 0;
         }
         else if (t >= ms->totalDuration)
         {
            hoveredSegment = std::max(0, ms->n_activeSegments - 1);
         }
         else
         {
            hoveredSegment = Surge::MSEG::timeToSegment(ms, t);
         }
         if (hoveredSegment != ohs)
            invalid();
      }

      if( inDrawDrag )
      {
         auto tf = pxToTime( );
         auto t = tf( where.x );
         auto pv = pxToVal();
         auto v = limit_range( pv( where.y ), -1.f, 1.f );

         int seg = Surge::MSEG::timeToSegment( this->ms, t );

         if( seg >= 0 && seg < ms->n_activeSegments && t <= ms->totalDuration + MSEGStorage::minimumDuration )
         {
            bool ch = false;
            // OK are we near the endpoint of that segment
            if( t - ms->segmentStart[seg] < std::max( ms->totalDuration * 0.05, 0.1 * ms->segments[seg].duration ) )
            {
               ms->segments[seg].v0 = v;
               ch = true;
            }
            else if( ms->segmentEnd[seg] - t < std::max( ms->totalDuration * 0.05, 0.1 * ms->segments[seg].duration ) )
            {
               int nx = seg + 1;
               if( ms->endpointMode == MSEGStorage::EndpointMode::FREE )
               {
                  if( nx == ms->n_activeSegments  )
                     ms->segments[seg].nv1 = v;
                  else
                     ms->segments[nx].v0 = v;
               }
               else
               {
                  if( nx >= ms->n_activeSegments )
                     nx = 0;
                  ms->segments[nx].v0 = v;
               }
               ch = true;
            }
            if( ch )
            {
               modelChanged(seg);
            }
         }

         
         return kMouseEventHandled;
      }

      bool flip = false;
      for( auto &h : hotzones )
      {
         if( h.dragging ) {
            if( h.associatedSegment >= 0 )
               hoveredSegment = h.associatedSegment;
            continue;
         }
         
         if( h.rect.pointInside(where) )
         {
            if( ! h.active )
            {
               h.active = true;
               flip = true;
            }

            if( h.associatedSegment >= 0 )
               hoveredSegment = h.associatedSegment;
         }
         else
         {
            if( h.active )
            {
               h.active = false;
               flip = true;
            }
         }
      }
      
      if( flip )
         invalid();

      if( inDrag )
      {
         bool gotOne = false;
         int idx = 0;
         for( auto &h : hotzones )
         {
            if( h.dragging )
            {
               if( cursorHideEnqueued )
               {
                  startCursorHide(cursorHideOrigin);
                  cursorHideEnqueued = false;
               }
               gotOne = true;
               float dragX = where.x - mouseDownOrigin.x;
               float dragY = where.y - mouseDownOrigin.y;
               if( buttons & kShift )
               {
                  dragX *= 0.2;
                  dragY *= 0.2;
               }
               h.onDrag( dragX, dragY, where );
#if ! LINUX
               float dx = where.x - cursorHideOrigin.x;
               float dy = where.y - cursorHideOrigin.y;
               if( dx * dx + dy * dy > 100 )
               {
                  resetToShowLocation();
                  mouseDownOrigin = cursorHideOrigin;
               }
               else
               {
                  mouseDownOrigin = where;
               }
#else
               mouseDownOrigin = where;
#endif
               modelChanged(h.associatedSegment, h.specialEndpoint); // HACK FIXME
               break;
            }
            idx++;
         }
         if (gotOne)
         {
            Surge::MSEG::rebuildCache(ms);
            recalcHotZones(where);
            hotzones[idx].dragging = true;
            invalid();
         }
         else if (buttons & kLButton || buttons & kMButton)
         {
            // This means we are a pan or zoom gesture
            float x = where.x - mouseDownOrigin.x;
            float y = where.y - mouseDownOrigin.y;
            float r = sqrt(x * x + y * y);
            if (r > 3)
            {
               if (fabs(x) > fabs(y))
               {
                  float dx = where.x - lastPanZoomMousePos.x;
                  float panScale = 1.0 / getDrawArea().getWidth();
                  pan(where, -dx * panScale, buttons );
               }
               else
               {
                  float dy = where.y - lastPanZoomMousePos.y;
                  float zoomScale = 2. / getDrawArea().getHeight();
                  zoom( where, -dy * zoomScale, buttons );
               }

               lastPanZoomMousePos = where;
            }
         }

         /*
          * Activate temporary snap. Note this is also checked in onMouseDown
          * so if you change ctrl/alt here change it there too
          */
         bool c = buttons & kControl;
         bool a = buttons & kAlt;
         if( ( c || a ) )
         {
            bool wasSnapGuard = true;
            if( ! snapGuard )
            {
               wasSnapGuard = false;
               snapGuard = std::make_shared<SnapGuard>(eds, this);
            }
            if( c ) eds->hSnap = eds->hSnapDefault;
            else if( wasSnapGuard ) eds->hSnap = snapGuard->hSnapO;

            if( a ) eds->vSnap = eds->vSnapDefault;
            else if( wasSnapGuard ) eds->vSnap = snapGuard->vSnapO;
         }
         else if( ! ( c || a ) && snapGuard )
         {
            snapGuard = nullptr;
         }

      }
      
      return kMouseEventHandled;
   }

   int wheelAxisCount = 0;
   bool onWheel(const CPoint& where,
                const CMouseWheelAxis& axis,
                const float& distance,
                const CButtonState& buttons) override
   {
      // fixmes - most recent
      if( axis == CMouseWheelAxis::kMouseWheelAxisY )
      {
         wheelAxisCount++;
      }
      else
      {
         wheelAxisCount--;
      }
      wheelAxisCount = limit_range(wheelAxisCount, -3, 3 );
      if(wheelAxisCount <= -2 )
      {
         panWheel( where, distance, buttons );
      }
      else
      {
         zoomWheel( where, distance, buttons );
      }
      return true;
   }

   float axisStart = 0.0;
   float axisWidth = 1.0;

   float lastZoomDir = 0;

   void zoomWheel( const CPoint &where, float amount, const CButtonState &buttons  )
   {
#if MAC
      if ((lastZoomDir < 0 && amount > 0) || (lastZoomDir > 0 && amount < 0))
      {
         lastZoomDir = amount;
         return;
      }
      lastZoomDir = amount;
#endif
      zoom(where, amount * 0.1, buttons );
   }

   void zoom( const CPoint &where, float amount, const CButtonState &buttons )
   {
      if( fabs(amount) < 1e-4 ) return;
      float dWidth = amount * axisWidth;
      auto pxt = pxToTime();
      float t = pxt(where.x);

      axisWidth = axisWidth - dWidth;

      /*
       * OK so we want to adjust pan so the mouse position is basically the same
       * - this is pxToTime
         auto drawArea = getDrawArea();
         float maxt = drawDuration();
         float tscale = 1.f * drawArea.getWidth() / maxt;

         // So px = t * tscale + drawarea;
         // So t = ( px - drawarea ) / tscale;
         return [tscale, drawArea, this ](float px) {
           return (px - drawArea.left) / tscale + axisStart;
         };

       * so we want axisStart to mean that (where.x - drawArea.left ) * drawDuration() / drawArea.width + as to be constant
       *
       * t = (WX-DAL) * DD / DAW + AS
       * AS = t - (WX_DAL) * DD / DAW
       *
       */
      auto DD = drawDuration();
      auto DA = getDrawArea();
      auto DAW = DA.getWidth();
      auto DAL = DA.left;
      auto WX = where.x;
      axisStart = std::max( t - ( WX - DAL ) * DD / DAW, 0. );
      applyZoomPanConstraints();

      // BOOKMARK
      recalcHotZones(where);
      invalid();
      // std::cout << "ZOOM by " << amount <<  " AT " << where.x << " " << t << std::endl;
   }

   void zoomToFull()
   {
      zoomOutTo( ms->totalDuration );
   }

   void zoomOutTo(float duration)
   {
      axisStart = 0.f;
      axisWidth = (ms->editMode == MSEGStorage::EditMode::ENVELOPE) ? std::max(1.0f, duration) : 1.f;
      modelChanged(0, false);
   }

   // On the mac I get occasional jutters where you get ---+--- or what not
   float lastPanDir = 0;

   void panWheel( const CPoint &where, float amount, const CButtonState &buttons  )
   {
#if MAC
      if ((lastPanDir < 0 && amount > 0) || (lastPanDir > 0 && amount < 0))
      {
         lastPanDir = amount;
         return;
      }
      lastPanDir = amount;
#endif
      pan(where, amount * 0.1, buttons);
   }

   void pan( const CPoint &where, float amount, const CButtonState &buttons ) {
      // std::cout << "PAN " << axisStart << " " << axisWidth << std::endl;
      axisStart += axisWidth * amount;
      axisStart = std::max( axisStart, 0.f );
      applyZoomPanConstraints();

      recalcHotZones(where);
      invalid();
   }

   void openPopup(const VSTGUI::CPoint &iw) {
      CPoint w = iw;
      localToFrame(w);

      COptionMenu* contextMenu = new COptionMenu(CRect( w, CPoint(0,0)), 0, 0, 0, 0,
                                                 VSTGUI::COptionMenu::kNoDrawStyle | VSTGUI::COptionMenu::kMultipleCheckStyle);

      auto tf = pxToTime( );
      auto t = tf( iw.x );
      auto tts = Surge::MSEG::timeToSegment(ms, t );

      if( hoveredSegment >= 0 && tts != hoveredSegment )
      {
         tts = hoveredSegment;
         t = ms->segmentStart[tts];
      }

      auto addCb = [](COptionMenu *p, const std::string &l, std::function<void()> op ) -> CCommandMenuItem * {
                      auto m = new CCommandMenuItem( CCommandMenuItem::Desc( l.c_str() ) );
                      m->setActions([op](CCommandMenuItem* m) { op(); });
                      p->addEntry( m );
                      return m;
                   };

      contextMenu->addEntry( "[?] MSEG Segment" );

      contextMenu->addSeparator();

      auto cbStart = addCb(contextMenu, Surge::UI::toOSCaseForMenu("Set Loop Start"), [this, tts]() {
         if (ms->loop_end == -1)
            ms->loop_start = tts;
         else
            ms->loop_start = std::min(ms->loop_end + 1, tts);
         modelChanged();
      });
      cbStart->setEnabled(ms->editMode != MSEGStorage::LFO );

      auto cbEnd = addCb(contextMenu, Surge::UI::toOSCaseForMenu("Set Loop End"), [this, tts, t]() {
         auto along = t - ms->segmentStart[tts];
         if( ms->segments[tts].duration == 0 )
            along = 0;
         else
            along = along / ms->segments[tts].duration;

         int target = tts;
         if( along < 0.1 && tts > 0 ) target = tts - 1;

         ms->loop_end = target;
         modelChanged();
      });
      cbEnd->setEnabled( ms->editMode != MSEGStorage::LFO );

      contextMenu->addSeparator();

      if( tts >= 0 )
      {
         COptionMenu* actionsMenu = new COptionMenu(CRect(w, CPoint(0, 0)), 0, 0, 0, 0,
                                                    VSTGUI::COptionMenu::kNoDrawStyle |
                                                    VSTGUI::COptionMenu::kMultipleCheckStyle);

         auto pv = pxToVal();
         auto v = pv( iw.y );
         
         addCb(actionsMenu, "Split",
                            [this, t, v](){
                                             Surge::MSEG::splitSegment( this->ms, t, v );
                                             modelChanged();
                                          });
         auto deleteMenu = addCb(actionsMenu, "Delete", 
                                              [this, t](){
                                                            Surge::MSEG::deleteSegment( this->ms, t );
                                                            modelChanged();
                                                         });
         if (ms->n_activeSegments <= 1)
            deleteMenu->setEnabled(false);
         
         actionsMenu->addSeparator();

         addCb(actionsMenu, Surge::UI::toOSCaseForMenu("Double Size"),
                            [this](){
                                       Surge::MSEG::scaleDurations(this->ms, 2.0);
                                       modelChanged();
                                    });
         addCb(actionsMenu, Surge::UI::toOSCaseForMenu("Half Size"),
                            [this](){
                                       Surge::MSEG::scaleDurations(this->ms, 0.5);
                                       modelChanged();
                                    });
         
         actionsMenu->addSeparator();

         addCb(actionsMenu, Surge::UI::toOSCaseForMenu("Flip Vertically"),
                            [this](){
                                       Surge::MSEG::scaleValues(this->ms, -1);
                                       modelChanged();
                                    });
         addCb(actionsMenu, Surge::UI::toOSCaseForMenu("Flip Horizontally"),
                            [this]() {
                                        Surge::MSEG::mirrorMSEG(this->ms);
                                        modelChanged();
                                     });

         actionsMenu->addSeparator();

         addCb(actionsMenu, Surge::UI::toOSCaseForMenu("Quantize Nodes to Snap Divisions"),
                            [this](){
                                       Surge::MSEG::setAllDurationsTo(this->ms, eds->hSnapDefault);
                                       modelChanged();
                                    });
         addCb(actionsMenu, Surge::UI::toOSCaseForMenu("Quantize Nodes to Whole Units"),
                            [this](){
                                       Surge::MSEG::setAllDurationsTo(this->ms, 1.0);
                                       modelChanged();
                                    });
         addCb(actionsMenu, Surge::UI::toOSCaseForMenu("Distribute Nodes Evenly"),
                            [this](){  
                                       auto totalLen = 0.f;
                                       for (int i = 0; i < this->ms->n_activeSegments; i++)
                                          totalLen += this->ms->segments[i].duration;

                                       Surge::MSEG::setAllDurationsTo(this->ms, totalLen / this->ms->n_activeSegments);
                                       modelChanged();
                                    });

         contextMenu->addEntry(actionsMenu, "Actions");


         COptionMenu* createMenu = new COptionMenu(CRect(w, CPoint(0, 0)), 0, 0, 0, 0,
                                                   VSTGUI::COptionMenu::kNoDrawStyle |
                                                   VSTGUI::COptionMenu::kMultipleCheckStyle);


         contextMenu->addEntry(createMenu, "Create");
         
         addCb(createMenu, Surge::UI::toOSCaseForMenu("Default MSEG"), [this]()
                           {
                              Surge::MSEG::createInitMSEG(this->ms);
                              zoomOutTo(1.f);
                           });

         createMenu->addSeparator();

         int stepCounts[3] = {8, 16, 32};

         for (int i : stepCounts)
         {
            addCb(createMenu, Surge::UI::toOSCaseForMenu(std::to_string(i) + " Step Sequencer"), [this, stepCounts, i]()
                {
                   Surge::MSEG::createStepseqMSEG(this->ms, i);
                   this->zoomOutTo(i);    
                });
         }

         createMenu->addSeparator();

         for (int i : stepCounts)
         {
            addCb(createMenu, Surge::UI::toOSCaseForMenu(std::to_string(i) + " Sawtooth Plucks"), [this, stepCounts, i]()
                {
                   Surge::MSEG::createSawMSEG(this->ms, i, 0.5);
                   this->zoomOutTo(i);
                });
         }

         createMenu->addSeparator();
         for (int i : stepCounts)
         {
            addCb(createMenu, Surge::UI::toOSCaseForMenu(std::to_string(i) + " Lines On Sin"),
                  [this, stepCounts, i] {
                     Surge::MSEG::createSinLineMSEG(this->ms, i);
                     this->zoomToFull();
                     modelChanged();
                  });
         }

         COptionMenu* settingsMenu = new COptionMenu(CRect(w, CPoint(0, 0)), 0, 0, 0, 0,
                                                     VSTGUI::COptionMenu::kNoDrawStyle |
                                                     VSTGUI::COptionMenu::kMultipleCheckStyle);

         auto cm = addCb(settingsMenu, Surge::UI::toOSCaseForMenu("Link Start and End Nodes"),
                         [this]() {
                             if( this->ms->endpointMode == MSEGStorage::EndpointMode::LOCKED )
                                 this->ms->endpointMode = MSEGStorage::EndpointMode::FREE;
                             else
                             {
                                 this->ms->endpointMode = MSEGStorage::EndpointMode::LOCKED;
                                 this->ms->segments[ms->n_activeSegments-1].nv1 = this->ms->segments[0].v0;
                                 modelChanged();
                             }
                         });
         cm->setChecked( this->ms->endpointMode == MSEGStorage::EndpointMode::LOCKED || this->ms->editMode == MSEGStorage::LFO);
         cm->setEnabled( this->ms->editMode != MSEGStorage::LFO );
 
         settingsMenu->addSeparator();
 
         auto def = ms->segments[tts].useDeform;
         auto dm = addCb(settingsMenu, Surge::UI::toOSCaseForMenu("Deform Applied to Segment"), [this, tts]() {
            this->ms->segments[tts].useDeform = ! this->ms->segments[tts].useDeform;
            modelChanged();
         });
         dm->setChecked(def);

         auto invdef = ms->segments[tts].invertDeform;
         auto im = addCb(settingsMenu, Surge::UI::toOSCaseForMenu("Invert Deform Value"), [this, tts]() {
            this->ms->segments[tts].invertDeform = ! this->ms->segments[tts].invertDeform;
            modelChanged();
         });
         im->setChecked(invdef);

         contextMenu->addEntry(settingsMenu, "Settings");

         contextMenu->addSeparator();

         auto typeTo = [this, contextMenu, t, addCb, tts](std::string n, MSEGStorage::segment::Type type) {
                          auto m = addCb( contextMenu, n, [this,t, type]() {
                                                    Surge::MSEG::changeTypeAt( this->ms, t, type );
                                                    modelChanged();
                                                 } );
                          if( tts >= 0 )
                             m->setChecked( this->ms->segments[tts].type == type );
                       };
         typeTo("Hold", MSEGStorage::segment::Type::HOLD);
         typeTo("Linear", MSEGStorage::segment::Type::LINEAR);
         typeTo("Bezier", MSEGStorage::segment::Type::QUAD_BEZIER);
         typeTo(Surge::UI::toOSCaseForMenu("S-Curve"), MSEGStorage::segment::Type::SCURVE);
         typeTo("Bump", MSEGStorage::segment::Type::BUMP);
         typeTo("Sine", MSEGStorage::segment::Type::SINE);
         typeTo("Sawtooth", MSEGStorage::segment::Type::SAWTOOTH);
         typeTo("Triangle", MSEGStorage::segment::Type::TRIANGLE);
         typeTo("Square", MSEGStorage::segment::Type::SQUARE);
         typeTo("Stairs", MSEGStorage::segment::Type::STAIRS);
         typeTo(Surge::UI::toOSCaseForMenu("Smooth Stairs"), MSEGStorage::segment::Type::SMOOTH_STAIRS);
         typeTo(Surge::UI::toOSCaseForMenu("Brownian Bridge"), MSEGStorage::segment::Type::BROWNIAN);
        
         getFrame()->addView( contextMenu );
         contextMenu->setDirty();
         contextMenu->popup();
         getFrame()->removeView(contextMenu, true);
      }
   }

   void modelChanged(int activeSegment = -1, bool specialEndpoint = false)
   {
      Surge::MSEG::rebuildCache( ms );
      applyZoomPanConstraints(activeSegment, specialEndpoint);
      recalcHotZones(mouseDownOrigin); // FIXME
      // Do this more heavy handed version
      getFrame()->invalid();
      // invalid();
   }

   void applyZoomPanConstraints(int activeSegment = -1, bool specialEndpoint = false)
   {
      if( ms->editMode == MSEGStorage::LFO )
      {
         // Reset axis bounds
         if( axisWidth > 1 )
            axisWidth = 1;
         if( axisStart + axisWidth > 1 )
            axisStart = 1.0 - axisWidth;
         if( axisStart < 0 )
            axisStart = 0;
      }
      else
      {
         auto bd = std::max( ms->totalDuration, 1.f );
         auto longest = bd * 2;
         if( longest > 128 ) longest = 128;
         if( longest < 32 ) longest = 32;
         if( axisWidth > longest )
         {
            axisWidth = longest;
         }
         else if( axisStart + axisWidth > longest )
         {
            axisStart = longest - axisWidth;
         }

         // This is how we pan as we stretch
         if( activeSegment >= 0 )
         {
            if( specialEndpoint )
            {
               if (ms->segmentEnd[activeSegment] >= axisStart + axisWidth)
               {
                  // In this case we are dragging the endpoint
                  axisStart = ms->segmentEnd[activeSegment] - axisWidth;
               }
               else if( ms->segmentEnd[activeSegment] <= axisStart )
               {
                  axisStart = ms->segmentEnd[activeSegment];
               }
            }
            else if( ms->segmentStart[activeSegment] >= axisStart + axisWidth  )
            {
               axisStart = ms->segmentStart[activeSegment] - axisWidth;
            }
            else if( ms->segmentStart[activeSegment] <= axisStart )
            {
               axisStart = ms->segmentStart[activeSegment];
            }
         }
      }
      axisWidth = std::max( axisWidth, 0.05f );
   }

   int hoveredSegment = -1;
   MSEGStorage *ms;
   MSEGEditor::State *eds;
   LFOStorage *lfodata;
   MSEGControlRegion *controlregion = nullptr;

   CScalableBitmap *handleBmp;
   
   CLASS_METHODS( MSEGCanvas, CControl );
};



void MSEGControlRegion::valueChanged( CControl *p )
{
   auto tag = p->getTag();
   auto val = p->getValue();

   switch (tag)
   {
   case tag_edit_mode:
   {
      int m = val > 0.5 ? 1 : 0;
      auto editMode = (MSEGStorage::EditMode)m;
      Surge::MSEG::modifyEditMode(this->ms, editMode );

      // zoom to fit
      canvas->axisStart = 0.f;
      canvas->axisWidth = editMode ? 1.f : ms->envelopeModeDuration;

      canvas->modelChanged(0, false);

      break;
   }
   case tag_loop_mode:
   {
      int m = floor((val * 2) + 0.1) + 1;
      ms->loopMode = (MSEGStorage::LoopMode)m;
      canvas->modelChanged();

      break;
   }
   case tag_segment_movement_mode:
   {
      int m = floor((val * 2) + 0.5);
      
      eds->timeEditMode = m;

      canvas->timeEditMode = (MSEGCanvas::TimeEdit)m;
      canvas->recalcHotZones(CPoint(0, 0));
      canvas->invalid();

      break;
   }
   case tag_horizontal_snap:
   {
      eds->hSnap = (val < 0.5) ? 0.f : eds->hSnap = eds->hSnapDefault;
      canvas->invalid();

      break;
   }
   case tag_vertical_snap:
   {
      eds->vSnap = (val < 0.5) ? 0.f : eds->vSnap = eds->vSnapDefault;
      canvas->invalid();

      break;
   }
   case tag_vertical_value:
   {
      auto fv = 1.f / std::max(1, static_cast<CNumberField*>(p)->getIntValue());
      eds->vSnapDefault = fv;
      if (eds->vSnap > 0)
         eds->vSnap = eds->vSnapDefault;
      canvas->invalid();

      break;
   }
   case tag_horizontal_value:
   {
      auto fv = 1.f / std::max(1, static_cast<CNumberField*>(p)->getIntValue());
      eds->hSnapDefault = fv;
      if (eds->hSnap > 0)
         eds->hSnap = eds->hSnapDefault;
      canvas->invalid();

      break;
   }
   default:
      break;
   }
}

int32_t MSEGControlRegion::controlModifierClicked(CControl* pControl, CButtonState button)
{
   int tag = pControl->getTag();

   // Basically all the menus are a list of options with values
   std::vector<std::pair<std::string,float>> options;

   /*  tag_loop_mode,
       tag_edit_mode,
       */

   bool isOnOff = false;
   std::string menuName = "";
   switch (tag)
   {
   case tag_segment_movement_mode:
      menuName = "Movement Mode";
      options.push_back(std::make_pair("Single", 0));
      options.push_back(std::make_pair("Shift", 0.5));
      options.push_back(std::make_pair("Draw", 1.0));
      break;

   case tag_loop_mode:
      menuName = "Loop Mode";
      options.push_back(std::make_pair("Off", 0));
      options.push_back(std::make_pair("Loop", 0.5));
      options.push_back(
          std::make_pair(Surge::UI::toOSCaseForMenu("Gate (Loop Until Release)").c_str(), 1.0));
      break;

   case tag_edit_mode:
      menuName = "Edit Mode";
      options.push_back(std::make_pair("Envelope", 0));
      options.push_back(std::make_pair("LFO", 1.0));
      break;

   case tag_vertical_snap:
      menuName = "Vertical Snap";
   case tag_horizontal_snap:
      if (menuName == "")
         menuName = "Horizontal Snap";
      isOnOff = true;
      break;

   case tag_vertical_value:
      menuName = "Vertical Snap Value";
   case tag_horizontal_value:
   {
       if (menuName == "")
         menuName = "Horizontal Snap Value";

      auto addStop = [&options](int v) {
         options.push_back(
             std::make_pair(std::to_string(v), Parameter::intScaledToFloat(v, 100, 1)));
      };

      addStop(2);
      addStop(3);
      addStop(4);
      addStop(5);
      addStop(6);
      addStop(7);
      addStop(8);
      addStop(9);
      addStop(10);
      addStop(12);
      addStop(16);
      addStop(24);
      addStop(32);

      break;
   }
   default:
      break;
   }

   if( options.size() || isOnOff )
   {
      VSTGUI::CPoint where;
      getFrame()->getCurrentMouseLocation(where);
      auto *com = new COptionMenu(CRect(where,CPoint()), nullptr, 0, 0,
                                  0, VSTGUI::COptionMenu::kNoDrawStyle | VSTGUI::COptionMenu::kMultipleCheckStyle);
      auto addcb = [com](std::string label, auto action){
         CCommandMenuItem* menu = new CCommandMenuItem(CCommandMenuItem::Desc(label.c_str()));
         menu->setActions([action](CCommandMenuItem* m) { action(); });
         com->addEntry(menu);
         return menu;
      };
      auto msurl = SurgeGUIEditor::helpURLForSpecial(storage, "mseg-editor" );
      auto hurl = SurgeGUIEditor::fullyResolvedHelpURL( msurl );

      addcb( "[?] " + menuName, [hurl](){
         Surge::UserInteractions::openURL(hurl);
      } );
      com->addSeparator();
      if( isOnOff )
      {
         if( pControl->getValue() > 0.5 )
         {
            addcb(Surge::UI::toOSCaseForMenu("Edit Value") + ": Off", [pControl, this]() {
               pControl->setValue( 0 );
               pControl->valueChanged();
               pControl->invalid();
               canvas->invalid();
               invalid();
            });
         }
         else
         {
            addcb(Surge::UI::toOSCaseForMenu("Edit Value") + ": On", [pControl, this]() {
               pControl->setValue( 1 );
               pControl->valueChanged();
               pControl->invalid();
               canvas->invalid();
               invalid();
            });
         }
      }
      else
      {
         for (auto op : options)
         {
            auto val = op.second;
            auto men = addcb(op.first, [val, pControl, this]() {
               pControl->setValue(val);
               pControl->invalid();
               pControl->valueChanged();
            });
            if (val == pControl->getValue())
               men->setChecked(true);
         }
      }
      getFrame()->addView(com);
      com->popup();
      getFrame()->removeView(com, true);

   }
   return 1;
}

void MSEGControlRegion::rebuild()
{
   auto labelFont = new VSTGUI::CFontDesc("Lato", 9, kBoldFace);
   auto editFont = new VSTGUI::CFontDesc("Lato", 9);
   
   int height = getViewSize().getHeight();
   int margin = 2;
   int labelHeight = 12;
   int buttonHeight = 14;
   int numfieldHeight = 12;
   int xpos = 10;

   // movement modes
   {
      int segWidth = 110;
      int marginPos = xpos + margin;
      int btnWidth = 94;
      int ypos = 1;

      // label
      auto mml = new CTextLabel(CRect(CPoint(marginPos, ypos), CPoint(btnWidth, labelHeight)), "Movement Mode");
      mml->setTransparency(true);
      mml->setFont(labelFont);
      mml->setFontColor(skin->getColor(Colors::MSEGEditor::Text));
      mml->setHoriAlign(kLeftText);
      addView(mml);

      ypos += margin + labelHeight;

      // button
      auto btnrect = CRect(CPoint(marginPos, ypos - 1), CPoint(btnWidth, buttonHeight));
      auto mw =
          new CHSwitch2(btnrect, this, tag_segment_movement_mode, 3, buttonHeight, 1, 3,
                        associatedBitmapStore->getBitmap(IDB_MSEG_MOVEMENT_MODE), CPoint(0, 0), true);
      mw->setSkin(skin, associatedBitmapStore);
      addView(mw);
      mw->setValue(canvas->timeEditMode / 2.f);

      // this value centers the loop mode and snap sections against the MSEG editor width
      // if more controls are to be added to that center section, reduce this value
      xpos += 173;
   }

   // edit mode
   {
      int segWidth = 107;
      int btnWidth = 90;
      int ypos = 1;

      auto eml = new CTextLabel(CRect(CPoint(xpos, ypos), CPoint(segWidth, labelHeight)), "Edit Mode");
      eml->setFont(labelFont);
      eml->setFontColor(skin->getColor(Colors::MSEGEditor::Text));
      eml->setTransparency(true);
      eml->setHoriAlign(kLeftText);
      addView(eml);

      ypos += margin + labelHeight;

      // button
      auto btnrect = CRect(CPoint(xpos, ypos - 1), CPoint(btnWidth, buttonHeight));
      auto ew = new CHSwitch2(btnrect, this, tag_edit_mode, 2, buttonHeight, 1, 2,
                        associatedBitmapStore->getBitmap(IDB_MSEG_EDIT_MODE), CPoint(0, 0), true);
      ew->setSkin(skin, associatedBitmapStore);
      addView(ew);
      ew->setValue(ms->editMode);

      xpos += segWidth;
   }

   // loop mode
   {
      int segWidth = 110;
      int btnWidth = 94;
      int ypos = 1;

      auto lpl = new CTextLabel(CRect(CPoint(xpos, ypos), CPoint(segWidth, labelHeight)), "Loop Mode");
      lpl->setFont(labelFont);
      lpl->setFontColor(skin->getColor(Colors::MSEGEditor::Text));
      lpl->setTransparency(true);
      lpl->setHoriAlign(kLeftText);
      addView(lpl);

      ypos += margin + labelHeight;

      // button
      auto btnrect = CRect(CPoint(xpos, ypos - 1), CPoint(btnWidth, buttonHeight));
      auto lw = new CHSwitch2(btnrect, this, tag_loop_mode, 3, buttonHeight, 1, 3,
                        associatedBitmapStore->getBitmap(IDB_MSEG_LOOP_MODE), CPoint(0, 0), true);
      lw->setSkin(skin, associatedBitmapStore);
      addView(lw);
      lw->setValue((ms->loopMode - 1) / 2.f);

      xpos += segWidth;
   }
   
   // snap to grid
   {
      int btnWidth = 49, editWidth = 32;
      int margin = 2;
      int segWidth = btnWidth + editWidth + 10;
      int ypos = 1;
      char svt[256];

      auto snapC = new CTextLabel(CRect(CPoint(xpos, ypos), CPoint(segWidth * 2 - 5, labelHeight)), "Snap To Grid");
      snapC->setTransparency(true);
      snapC->setFont(labelFont);
      snapC->setFontColor(skin->getColor(Colors::MSEGEditor::Text));
      snapC->setHoriAlign(kLeftText);
      addView(snapC);

      ypos += margin + labelHeight;

      auto hbut = new CSwitchControl(CRect(CPoint(xpos, ypos - 1), CPoint(btnWidth, buttonHeight)), this, tag_horizontal_snap,
                                     associatedBitmapStore->getBitmap(IDB_MSEG_HORIZONTAL_SNAP));
      hbut->setSkin(skin, associatedBitmapStore);
      addView(hbut);
      hbut->setValue(eds->hSnap < 0.001 ? 0 : 1);

      snprintf(svt, 255, "%d", (int)round(1.f / eds->hSnapDefault));
      

      /*
       * CNF responds to skin objects and we are not skin driven here. We could do two things
       * 1. Add a lot of ifs to CNF
       * 2. Make a proxy skin control
       * I choose 2.
       */
      auto hsrect = CRect(CPoint(xpos + 52 + margin, ypos), CPoint(editWidth, numfieldHeight));
      auto cnfSkinCtrl = std::make_shared<Surge::UI::Skin::Control>();
      cnfSkinCtrl->allprops["bg_id"] = std::to_string( IDB_MSEG_SNAPVALUE_NUMFIELD );
      cnfSkinCtrl->allprops["text_color"] = Colors::MSEGEditor::NumberField::Text.name;
      cnfSkinCtrl->allprops["text_color.hover"] = Colors::MSEGEditor::NumberField::TextHover.name;

      auto* hnf = new CNumberField(hsrect, this, tag_horizontal_value, nullptr /*, ref to storage?*/);
      hnf->setControlMode(cm_mseg_snap_h);
      hnf->setSkin(skin, associatedBitmapStore, cnfSkinCtrl);
      hnf->setMouseableArea(hsrect);
      hnf->setIntValue( round( 1.f / eds->hSnapDefault ) );

      addView(hnf);

      xpos += segWidth;

      auto vbut = new CSwitchControl(CRect(CPoint(xpos, ypos - 1), CPoint(btnWidth, buttonHeight)), this, tag_vertical_snap,
                                     associatedBitmapStore->getBitmap(IDB_MSEG_VERTICAL_SNAP));
      vbut->setSkin(skin, associatedBitmapStore);
      addView( vbut );
      vbut->setValue( eds->vSnap < 0.001? 0 : 1 );

      snprintf(svt, 255, "%d", (int)round( 1.f / eds->vSnapDefault));

      auto vsrect = CRect(CPoint(xpos + 52 + margin, ypos), CPoint(editWidth, numfieldHeight));
      auto* vnf = new CNumberField(vsrect, this, tag_vertical_value, nullptr /*, ref to storage?*/);
      vnf->setControlMode(cm_mseg_snap_v);
      vnf->setSkin(skin, associatedBitmapStore, cnfSkinCtrl);
      vnf->setMouseableArea(vsrect);
      vnf->setIntValue( round( 1.f / eds->vSnapDefault ));

      addView(vnf);
   }
}

struct MSEGMainEd : public CViewContainer {

   MSEGMainEd(const CRect &size, SurgeStorage *storage, LFOStorage *lfodata, MSEGStorage *ms, MSEGEditor::State *eds, Surge::UI::Skin::ptr_t skin, std::shared_ptr<SurgeBitmaps> bmp) : CViewContainer(size) {
      this->ms = ms;
      this->skin = skin;

      int controlHeight = 35;

      auto msegCanv = new MSEGCanvas( CRect( CPoint( 0, 0 ), CPoint( size.getWidth(), size.getHeight() - controlHeight ) ), lfodata, ms, eds, skin, bmp );
            
      auto msegControl = new MSEGControlRegion(CRect( CPoint( 0, size.getHeight() - controlHeight ), CPoint(  size.getWidth(), controlHeight ) ), msegCanv,
                                               storage, lfodata, ms, eds, skin, bmp );


      msegCanv->controlregion = msegControl;
      msegControl->canvas = msegCanv;
      
      addView( msegCanv );
      addView( msegControl );
   }

   Surge::UI::Skin::ptr_t skin;
   MSEGStorage *ms;

};


MSEGEditor::MSEGEditor(SurgeStorage* storage, LFOStorage* lfodata, MSEGStorage* ms, State* eds, Surge::UI::Skin::ptr_t skin, std::shared_ptr<SurgeBitmaps> b)
    : CViewContainer(CRect(0, 0, 1, 1))
{
   auto npc = Surge::Skin::Connector::NonParameterConnection::MSEG_EDITOR_WINDOW;
   auto conn = Surge::Skin::Connector::connectorByNonParameterConnection(npc);
   auto skinCtrl = skin->getOrCreateControlForConnector(conn);

   setViewSize(CRect(CPoint(0, 0), CPoint(skinCtrl->w, skinCtrl->h)));

   // Leave these in for now
   if( ms->n_activeSegments <= 0 ) // This is an error state! Compensate
   {
      Surge::MSEG::createInitMSEG(ms);
   }
   setSkin( skin, b );
   setBackgroundColor( kRedCColor );
   addView( new MSEGMainEd( getViewSize(), storage, lfodata, ms, eds, skin, b ) );
}

MSEGEditor::~MSEGEditor() {
}
