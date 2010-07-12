// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINDOW_MANAGER_REAL_COMPOSITOR_H_
#define WINDOW_MANAGER_REAL_COMPOSITOR_H_

#include <cmath>
#include <list>
#include <map>
#include <set>
#include <string>
#include <tr1/memory>
#include <tr1/unordered_set>
#include <vector>

#include <gtest/gtest_prod.h>  // for FRIEND_TEST() macro

#include "base/hash_tables.h"
#include "base/logging.h"
#include "base/scoped_ptr.h"
#include "window_manager/compositor.h"
#include "window_manager/math_types.h"
#include "window_manager/x_types.h"

#if !(defined(COMPOSITOR_OPENGL) || defined(COMPOSITOR_OPENGLES))
#error COMPOSITOR_OPENGL or COMPOSITOR_OPENGLES must be defined
#endif

namespace window_manager {

class CompositorEventSource;
class EventLoop;
class Gles2Interface;
class GLInterface;
class OpenGlDrawVisitor;
class OpenGlesDrawVisitor;
class TextureData;
class XConnection;

class RealCompositor : public Compositor {
 public:
  class Actor;
  class ContainerActor;
  class ImageActor;
  class QuadActor;
  class StageActor;
  class TexturePixmapActor;

  typedef std::vector<Actor*> ActorVector;

  // This is in milliseconds.
  typedef int64_t AnimationTime;

  template<class T>
  class Animation {
   public:
    Animation(T* field, T end_value,
              AnimationTime start_time, AnimationTime end_time)
        : field_(field) {
      Reset(end_value, start_time, end_time);
    }
    ~Animation() {}

    // Reset the animation to use a new end value and duration.  The
    // field's current value is used as the start value.
    void Reset(T end_value, AnimationTime start_time, AnimationTime end_time) {
      start_value_ = *field_;
      end_value_ = end_value;
      start_time_ = start_time;
      end_time_ = end_time;
      ease_factor_ = M_PI / (end_time_ - start_time_);
    }

    // Evaluate the animation at the passed-in time and update the
    // field associated with it.  It returns true when the animation
    // is finished.
    bool Eval(AnimationTime current_time) {
      if (current_time >= end_time_) {
        *field_ = end_value_;
        return true;
      }
      float x = (1.0f - cosf(ease_factor_ * (current_time - start_time_))) /
          2.0f;
      *field_ = InterpolateValue(start_value_, end_value_, x);
      return false;
    }

   private:
    // Helper method for Eval() that interpolates between two points.
    // Integral types can specialize this to do rounding.
    static T InterpolateValue(T start_value, T end_value, float fraction) {
      return start_value + fraction * (end_value - start_value);
    }

    T* field_;
    T start_value_;
    T end_value_;

    AnimationTime start_time_;
    AnimationTime end_time_;

    float ease_factor_;

    DISALLOW_COPY_AND_ASSIGN(Animation);
  };

  class ActorVisitor {
   public:
    ActorVisitor() {}
    virtual ~ActorVisitor() {}
    virtual void VisitActor(Actor* actor) = 0;

    // Default implementation visits container as an Actor, and then
    // calls Visit on all the container's children.
    virtual void VisitContainer(ContainerActor* actor);
    virtual void VisitStage(StageActor* actor) { VisitContainer(actor); }
    virtual void VisitQuad(QuadActor* actor) { VisitActor(actor); }
    virtual void VisitImage(ImageActor* actor) { VisitActor(actor); }
    virtual void VisitTexturePixmap(TexturePixmapActor* actor) {
      VisitActor(actor);
    }
   private:
    DISALLOW_COPY_AND_ASSIGN(ActorVisitor);
  };

  class VisitorDestination {
   public:
    VisitorDestination() {}
    virtual ~VisitorDestination() {}

    // This function accepts a visitor into the destination to be
    // visited.
    virtual void Accept(ActorVisitor* visitor) = 0;
   private:
    DISALLOW_COPY_AND_ASSIGN(VisitorDestination);
  };

  class LayerVisitor : virtual public RealCompositor::ActorVisitor {
   public:
    static const float kMinDepth;
    static const float kMaxDepth;

    explicit LayerVisitor(int32 count)
        : depth_(0.0f),
          layer_thickness_(0.0f),
          count_(count),
          has_fullscreen_actor_(false) {}
    virtual ~LayerVisitor() {}

    virtual void VisitActor(RealCompositor::Actor* actor);
    virtual void VisitStage(RealCompositor::StageActor* actor);
    virtual void VisitContainer(RealCompositor::ContainerActor* actor);
    virtual void VisitQuad(RealCompositor::QuadActor* actor);
    virtual void VisitImage(RealCompositor::ImageActor* actor);
    virtual void VisitTexturePixmap(RealCompositor::TexturePixmapActor* actor);

    void VisitTexturedQuadActor(RealCompositor::QuadActor* actor,
                                bool is_texture_opaque);

    bool has_fullscreen_actor() const { return has_fullscreen_actor_; }

   private:
    float depth_;
    float layer_thickness_;
    int32 count_;
    bool has_fullscreen_actor_;

    DISALLOW_COPY_AND_ASSIGN(LayerVisitor);
  };

  class Actor : virtual public Compositor::Actor,
                public RealCompositor::VisitorDestination {
   public:
    explicit Actor(RealCompositor* compositor);
    virtual ~Actor();

    // Begin Compositor::VisitorDestination methods.
    virtual void Accept(ActorVisitor* visitor) {
      DCHECK(visitor);
      visitor->VisitActor(this);
    }
    // End Compositor::VisitorDestination methods.

    // Begin Compositor::Actor methods.
    virtual Actor* Clone();
    int GetWidth() { return width_; }
    int GetHeight() { return height_; }
    int GetX() { return x_; }
    int GetY() { return y_; }
    double GetXScale() { return scale_x_; }
    double GetYScale() { return scale_y_; }
    virtual void Show() { SetIsShown(true); }
    virtual void Hide() { SetIsShown(false); }
    void SetSize(int width, int height) {
      width_ = width;
      height_ = height;
      SetDirty();
    }
    void SetName(const std::string& name) { name_ = name; }
    const std::string& name() const { return name_; }

    void Move(int x, int y, int duration_ms);
    void MoveX(int x, int duration_ms);
    void MoveY(int y, int duration_ms);
    void Scale(double scale_x, double scale_y, int duration_ms);
    void SetOpacity(double opacity, int duration_ms);
    void SetTilt(double tilt, int duration_ms);
    double GetTilt() const { return tilt_; }

    void Raise(Compositor::Actor* other);
    void Lower(Compositor::Actor* other);
    void RaiseToTop();
    void LowerToBottom();
    virtual std::string GetDebugString(int indent_level) {
      return GetDebugStringInternal("Actor", indent_level);
    }
    virtual void ShowDimmed(bool dimmed, int anim_ms);
    virtual void AddToVisibilityGroup(int group_id);
    virtual void RemoveFromVisibilityGroup(int group_id);
    // End Compositor::Actor methods.

    // Updates the actor in response to time passing, and counts the
    // number of actors as it goes.
    virtual void Update(int32* count, AnimationTime now);

    // Updates the model view matrix associated with this actor.
    virtual void UpdateModelView();

    // Regular actors have no children, but we want to be able to
    // avoid a virtual function call to determine this while
    // traversing.
    bool has_children() const { return has_children_; }

    virtual ActorVector GetChildren() {
      return ActorVector();
    }

    void set_parent(ContainerActor* parent) { parent_ = parent; }
    ContainerActor* parent() const { return parent_; }
    int width() const { return width_; }
    int height() const { return height_; }
    int x() const { return x_; }
    int y() const { return y_; }
    void set_z(float z) { z_ = z; }
    float z() const { return z_; }

    // Note that is_opaque, culled, and model_view are not valid until after
    // a LayerVisitor has been run over the tree -- that's what calculates the
    // the opacity flag, updates model view matrix, and performs culling.

    // The model view matrix is derived from translation, scaling, rotation,
    // and tilt operations.  All actors should have model view matrices.
    const Matrix4& model_view() const { return model_view_; }
    void set_model_view(const Matrix4& model_view) {
      model_view_ = model_view;
    }
    bool is_opaque() const { return is_opaque_; }
    void set_is_opaque(bool opaque) { is_opaque_ = opaque; }
    bool culled() const { return culled_; }
    void set_culled(bool culled) { culled_ = culled; }

    // Checks if the actor is visible on screen and should be rendered.
    virtual bool IsVisible() const {
      return is_shown_ && !culled_ && opacity_ > 0.001f &&
             IsInActiveVisibilityGroup();
    }
    float opacity() const { return opacity_; }
    float tilt() const { return tilt_; }
    float scale_x() const { return scale_x_; }
    float scale_y() const { return scale_y_; }
    void SetDirty() { compositor_->SetDirty(); }

    bool is_dimmed() const { return dimmed_opacity_ > 0.001f; }
    float dimmed_opacity() const { return dimmed_opacity_; }

   protected:
    // Needs to update the opacity flag.
    friend class RealCompositor::LayerVisitor;

    RealCompositor* compositor() { return compositor_; }
    bool is_shown() const { return is_shown_; }
    void SetIsShown(bool is_shown) {
      if (is_shown_ == is_shown)
        return;
      is_shown_ = is_shown;
      SetDirty();
    }

    void CloneImpl(Actor* clone);

    // Helper method that can be invoked by derived classes.  Returns a
    // string defining this actor, saying that its type is 'type_name'
    // (e.g. "RectangleActor", "TexturePixmapActor", etc.).
    std::string GetDebugStringInternal(const std::string& type_name,
                                       int indent_level);

    void set_has_children(bool has_children) { has_children_ = has_children; }

   private:
    // Animate one of this actor's fields moving to a new value.
    // 'animation_map' is '&int_animations_' or '&float_animations_'.
    template<class T> void AnimateField(
        std::map<T*, std::tr1::shared_ptr<Animation<T> > >* animation_map,
        T* field, T value, int duration_ms);

    // Helper method called by Update() for 'int_animations_' and
    // 'float_animations_'.  Goes through the passed-in map, calling each
    // animation's Eval() method and deleting it if it's done.
    template<class T> void UpdateInternal(
        std::map<T*, std::tr1::shared_ptr<Animation<T> > >* animation_map,
        AnimationTime now);

    // Is this actor in a visibility group that's currently being drawn (or
    // are visibility groups disabled in the compositor)?
    bool IsInActiveVisibilityGroup() const;

    RealCompositor* compositor_;

    // Parent containing this actor.
    ContainerActor* parent_;

    // X- and Y-position relative to the parent's origin.
    int x_;
    int y_;

    // Width and height of the actor's bounding box.
    int width_;
    int height_;

    // Z-depth of this actor (set according to the layer this actor is on).
    float z_;

    // X- and Y-scale of the actor.
    float scale_x_;
    float scale_y_;

    // Opacity of the actor (0 = transparent, 1 = opaque).
    float opacity_;

    // The amount that the actor should be "tilted".  This is a
    // perspective effect where the actor is rotated around its left
    // edge.
    float tilt_;

    // Indicates if this actor has passed/failed the culling visibility test.
    bool culled_;

    // Cache model view matrix, so that it is only updated when something
    // changes and it can be reused.
    Matrix4 model_view_;

    // Calculated during the layer visitor pass, and used to determine
    // if this object is opaque for traversal purposes.
    bool is_opaque_;

    // This indicates if this actor has any children (false for all
    // but containers).  This is here so we can avoid a virtual
    // function call to determine this during the drawing traversal.
    bool has_children_;

    // This says whether or not to show this actor.
    bool is_shown_;

    // The opacity of the dimming quad.
    float dimmed_opacity_;

    // Name used for identifying the actor (useful for debugging).
    std::string name_;

    // Map from the address of a field to the animation that is modifying it.
    std::map<int*, std::tr1::shared_ptr<Animation<int> > > int_animations_;
    std::map<float*, std::tr1::shared_ptr<Animation<float> > >
        float_animations_;

    // IDs of visibility groups this actor is a member of.
    std::set<int> visibility_groups_;

    DISALLOW_COPY_AND_ASSIGN(Actor);
  };

  class ContainerActor : public RealCompositor::Actor,
                         virtual public Compositor::ContainerActor {
   public:
    explicit ContainerActor(RealCompositor* compositor)
        : RealCompositor::Actor(compositor) {
    }
    ~ContainerActor();

    // Implement VisitorDestination for visitor.
    void Accept(ActorVisitor* visitor) {
      CHECK(visitor);
      visitor->VisitContainer(this);
    }

    // Begin Compositor::Actor methods.
    virtual void SetSize(int width, int height) {
      LOG(WARNING) << "Ignoring request to set size of ContainerActor";
    }
    virtual Actor* Clone() {
      NOTIMPLEMENTED();
      CHECK(false);
      return NULL;
    }
    virtual std::string GetDebugString(int indent_level);
    // End Compositor::Actor methods.

    // Begin RealCompositor::Actor methods.
    virtual ActorVector GetChildren() { return children_; }

    // ContainerActor handles translation differently than other actors.
    virtual void UpdateModelView();
    // End RealCompositor::Actor methods.

    // Begin Compositor::ContainerActor methods.
    void AddActor(Compositor::Actor* actor);
    // End Compositor::ContainerActor methods.

    void RemoveActor(Compositor::Actor* actor);
    virtual void Update(int32* count, AnimationTime now);

    // Raise one child over another.  Raise to top if "above" is NULL.
    void RaiseChild(RealCompositor::Actor* child,
                    RealCompositor::Actor* above);
    // Lower one child under another.  Lower to bottom if "below" is NULL.
    void LowerChild(RealCompositor::Actor* child,
                    RealCompositor::Actor* below);

   private:
    // The list of this container's children.
    ActorVector children_;
    DISALLOW_COPY_AND_ASSIGN(ContainerActor);
  };

  // This class represents a quadrilateral.
  class QuadActor : public RealCompositor::Actor {
   public:
    explicit QuadActor(RealCompositor* compositor);

    // Begin Compositor::Actor methods.
    virtual std::string GetDebugString(int indent_level) {
      return GetDebugStringInternal("QuadActor", indent_level);
    }
    // End Compositor::Actor methods.

    void SetColor(const Compositor::Color& color,
                  const Compositor::Color& border_color,
                  int border_width) {
      DCHECK(border_width >= 0);
      color_ = color;
      border_color_ = color;
      border_width_ = border_width;
    }
    const Compositor::Color& color() const { return color_; }
    const Compositor::Color& border_color() const {
      return border_color_;
    }
    const int border_width() const { return border_width_; }

    TextureData* texture_data() const { return texture_data_.get(); }
    void set_texture_data(TextureData* texture_data) {
      texture_data_.reset(texture_data);
    }

    // Implement VisitorDestination for visitor.
    void Accept(ActorVisitor* visitor) {
      CHECK(visitor);
      visitor->VisitQuad(this);
    }

    virtual Actor* Clone();

   protected:
    void CloneImpl(QuadActor* clone);

   private:
    Compositor::Color color_;
    Compositor::Color border_color_;
    int border_width_;
    std::tr1::shared_ptr<TextureData> texture_data_;

    DISALLOW_COPY_AND_ASSIGN(QuadActor);
  };

  class ImageActor : public RealCompositor::QuadActor,
                     public Compositor::ImageActor {
   public:
    explicit ImageActor(RealCompositor* compositor);
    virtual ~ImageActor() {}

    // Begin Compositor::Actor methods.
    virtual void SetSize(int width, int height) {
      // ImageActors just track the size of their image data.
      LOG(WARNING) << "Ignoring request to set size of ImageActor";
    }
    virtual Actor* Clone();
    virtual std::string GetDebugString(int indent_level) {
      return GetDebugStringInternal("ImageActor", indent_level);
    }
    // End Compositor::Actor methods.

    // Begin Compositor::ImageActor methods.
    virtual void SetImageData(const ImageContainer& image_container);
    // End Compositor::ImageActor methods.

    // Implement VisitorDestination for visitor.
    virtual void Accept(ActorVisitor* visitor) {
      CHECK(visitor);
      visitor->VisitImage(this);
    }
    // End Compositor::TexturePixmapActor methods.
   private:
    DISALLOW_COPY_AND_ASSIGN(ImageActor);
  };

  class TexturePixmapActor : public RealCompositor::QuadActor,
                             public Compositor::TexturePixmapActor {
   public:
    explicit TexturePixmapActor(RealCompositor* compositor);
    virtual ~TexturePixmapActor();

    XID pixmap() const { return pixmap_; }
    bool pixmap_is_opaque() const { return pixmap_is_opaque_; }

    // Begin Compositor::Actor methods.
    virtual std::string GetDebugString(int indent_level) {
      return GetDebugStringInternal("TexturePixmapActor", indent_level);
    }
    // TexturePixmapActors just track the size of their pixmaps.
    virtual void SetSize(int width, int height) {
      LOG(WARNING) << "Ignoring request to set size of TexturePixmapActor";
    }
    virtual Actor* Clone() {
      NOTIMPLEMENTED();
      CHECK(false);
      return NULL;
    }
    // End Compositor::Actor methods.

    // Implement VisitorDestination for visitor.
    virtual void Accept(ActorVisitor* visitor) {
      CHECK(visitor);
      visitor->VisitTexturePixmap(this);
    }

    // Begin Compositor::TexturePixmapActor methods.
    virtual void SetPixmap(XID pixmap);
    virtual void UpdateTexture();
    virtual void SetAlphaMask(const uint8_t* bytes, int width, int height) {
      NOTIMPLEMENTED();
    }
    virtual void ClearAlphaMask() { NOTIMPLEMENTED(); }
    // End Compositor::TexturePixmapActor methods.

   private:
    FRIEND_TEST(RealCompositorTest, HandleXEvents);

    // Offscreen X pixmap whose contents we're displaying.
    XID pixmap_;

    // Is 'pixmap_' opaque (i.e. it has a non-32-bit depth)?
    bool pixmap_is_opaque_;

    DISALLOW_COPY_AND_ASSIGN(TexturePixmapActor);
  };

  class StageActor : public RealCompositor::ContainerActor,
                     public Compositor::StageActor {
   public:
    StageActor(RealCompositor* compositor, int width, int height);
    virtual ~StageActor();

    // Implement VisitorDestination for visitor.
    void Accept(ActorVisitor* visitor) {
      CHECK(visitor);
      visitor->VisitStage(this);
    }

    // Begin Compositor::Actor methods.
    virtual void SetSize(int width, int height);
    virtual Actor* Clone() {
      NOTIMPLEMENTED();
      return NULL;
    }
    // End Compositor::Actor methods.

    // Begin RealCompositor::Actor methods.
    // We don't want to bother with things like visibility groups or
    // opacity for the stage.
    virtual bool IsVisible() const { return is_shown(); }

    // StageActor does not update model view matrix, it updates projection.
    virtual void UpdateModelView() {}
    // End RealCompositor::Actor methods.

    // Begin Compositor::StageActor methods.
    XWindow GetStageXWindow() { return window_; }
    void SetStageColor(const Compositor::Color& color);
    void UpdateProjection();
    // End Compositor::StageActor methods.

    const Compositor::Color& stage_color() const { return stage_color_; }
    bool stage_color_changed() const { return stage_color_changed_; }
    void unset_stage_color_changed() { stage_color_changed_ = false; }

    bool was_resized() const { return was_resized_; }
    void unset_was_resized() { was_resized_ = false; }

    const Matrix4& projection() const { return projection_; }

   private:
    // This is the XWindow associated with the stage.  Owned by this class.
    XWindow window_;

    // Only StageActor has projection matrix.
    Matrix4 projection_;

    // Has the stage's color been changed?  This gets set by
    // SetStageColor() and checked and reset by the visitor.
    bool stage_color_changed_;

    // Has the stage been resized?  This gets set by SetSizeImpl() and
    // then checked and reset by the visitor after it resizes the viewport.
    bool was_resized_;

    Compositor::Color stage_color_;
    DISALLOW_COPY_AND_ASSIGN(StageActor);
  };

  RealCompositor(EventLoop* event_loop,
                 XConnection* x_conn,
#if defined(COMPOSITOR_OPENGL)
                 GLInterface* gl_interface
#elif defined(COMPOSITOR_OPENGLES)
                 Gles2Interface* gl_interface
#endif
                );
  ~RealCompositor();

  // Begin Compositor methods.
  virtual ContainerActor* CreateGroup();
  virtual Actor* CreateRectangle(const Compositor::Color& color,
                                 const Compositor::Color& border_color,
                                 int border_width);
  virtual ImageActor* CreateImage();
  virtual ImageActor* CreateImageFromFile(const std::string& filename);
  virtual TexturePixmapActor* CreateTexturePixmap();
  virtual Actor* CloneActor(Compositor::Actor* orig);
  virtual StageActor* GetDefaultStage() { return default_stage_.get(); }
  virtual void SetActiveVisibilityGroups(
      const std::tr1::unordered_set<int>& groups);

  // Run in-progress animations and redraw the scene if needed.  Disables
  // the draw timeout if there are no in-progress animations.
  virtual void Draw();
  // End Compositor methods

  XConnection* x_conn() { return x_conn_; }
  // TODO: These are just here so that ImageActor::SetImageData() can
  // update its texture.  Find a better way to expose this.
#if defined(COMPOSITOR_OPENGL)
  OpenGlDrawVisitor* draw_visitor() { return draw_visitor_.get(); }
#elif defined(COMPOSITOR_OPENGLES)
  OpenGlesDrawVisitor* draw_visitor() { return draw_visitor_.get(); }
#endif
  int actor_count() { return actor_count_; }
  bool dirty() const { return dirty_; }
  void set_current_time_ms_for_testing(int64_t time_ms) {
    current_time_ms_for_testing_ = time_ms;
  }
  bool using_visibility_groups() const {
    return !active_visibility_groups_.empty();
  }
  const std::tr1::unordered_set<int>& active_visibility_groups() const {
    return active_visibility_groups_;
  }

  // These accessors are present for testing.
  int draw_timeout_id() const { return draw_timeout_id_; }
  bool draw_timeout_enabled() const { return draw_timeout_enabled_; }

  void AddActor(Actor* actor) { actors_.push_back(actor); }
  void RemoveActor(Actor* actor);

  // Returns the current time, as milliseconds since the epoch, or
  // 'current_time_ms_for_testing_' if it's 0 or positive.
  AnimationTime GetCurrentTimeMs();

  // Mark the scene as dirty, enabling the draw timeout if needed.
  void SetDirty();

  // Increment or decrement the number of in-progress actors.  This is
  // invoked by Actor as animations start or stop.  The increment method
  // enables the draw timeout if needed.
  void IncrementNumAnimations();
  void DecrementNumAnimations();

 private:
  FRIEND_TEST(OpenGlVisitorTestTree, LayerDepth);  // sets actor count

  // Used by tests.
  void set_actor_count(int count) { actor_count_ = count; }

  // Enable or disable the draw timeout.  Safe to call if it's already
  // enabled/disabled.
  void EnableDrawTimeout();
  void DisableDrawTimeout();

  EventLoop* event_loop_;  // not owned
  XConnection* x_conn_;    // not owned

  // This indicates if the scene is dirty and needs to be redrawn.
  bool dirty_;

  // Total number of in-progress animations.
  int num_animations_;

  // This is the list of actors to display.
  ActorVector actors_;

  // This is the default stage where the actors are placed.
  scoped_ptr<StageActor> default_stage_;

  // This is the current time used to evaluate the currently active animations.
  AnimationTime now_;

  typedef base::hash_map<XWindow, TexturePixmapActor*>
      XIDToTexturePixmapActorMap;

  // This is a map that allows us to look up the texture associated
  // with an XWindow.
  XIDToTexturePixmapActorMap texture_pixmaps_;

  // This is the count of actors in the tree as of the last time
  // Update was called.  It is used to compute the depth delta for
  // layer depth calculations.
  int32 actor_count_;

#if defined(COMPOSITOR_OPENGL)
  scoped_ptr<OpenGlDrawVisitor> draw_visitor_;
#elif defined(COMPOSITOR_OPENGLES)
  scoped_ptr<OpenGlesDrawVisitor> draw_visitor_;
#endif

  // If 0 or positive, the time that will be returned by GetCurrentTimeMs().
  // Used for testing.
  int64_t current_time_ms_for_testing_;

  // Time that we last drew the scene, as milliseconds since the epoch.
  int64_t last_draw_time_ms_;

  // ID of the event loop timeout used to invoke Draw().
  int draw_timeout_id_;

  // Is the drawing timeout currently enabled?
  bool draw_timeout_enabled_;

  // Actor visibility groups that we're currently going to draw.  If empty,
  // we're not using visibility groups and just draw all actors.
  std::tr1::unordered_set<int> active_visibility_groups_;

  DISALLOW_COPY_AND_ASSIGN(RealCompositor);
};

// Specialization for integer animations that rounds to the nearest position.
template<>
inline int RealCompositor::Animation<int>::InterpolateValue(
    int start_value, int end_value, float fraction) {
  return roundf(start_value + fraction * (end_value - start_value));
}

}  // namespace window_manager

#endif  // WINDOW_MANAGER_REAL_COMPOSITOR_H_
