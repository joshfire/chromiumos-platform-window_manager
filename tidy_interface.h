// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINDOW_MANAGER_TIDY_INTERFACE_H_
#define WINDOW_MANAGER_TIDY_INTERFACE_H_

#include <cmath>
#include <list>
#include <map>
#include <string>
#include <tr1/memory>
#include <vector>

#include <gtest/gtest_prod.h>  // for FRIEND_TEST() macro

#include "base/hash_tables.h"
#include "base/logging.h"
#include "base/scoped_ptr.h"
#include "window_manager/clutter_interface.h"
#include "window_manager/x_types.h"

#if !(defined(TIDY_OPENGL) || defined(TIDY_OPENGLES))
#error TIDY_OPENGL or TIDY_OPENGLES must be defined
#endif

namespace window_manager {

class CompositorEventSource;
class EventLoop;
class GLInterfaceBase;
class OpenGlDrawVisitor;
class OpenGlesDrawVisitor;
class XConnection;

class TidyInterface : public ClutterInterface {
 public:
  class Actor;
  class ContainerActor;
  class DrawingData;
  class QuadActor;
  class StageActor;
  class TexturePixmapActor;

  typedef std::vector<Actor*> ActorVector;
  typedef std::tr1::shared_ptr<DrawingData> DrawingDataPtr;
  typedef std::map<int32, DrawingDataPtr> DrawingDataMap;

  // Base class for memento storage on the actors.
  class DrawingData {
   public:
    DrawingData() {}
    virtual ~DrawingData() {}
  };

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
    virtual void VisitStage(StageActor* actor) {
      VisitContainer(actor);
    }
    virtual void VisitTexturePixmap(TexturePixmapActor* actor) {
      VisitActor(actor);
    }
    virtual void VisitQuad(QuadActor* actor) {
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

  class LayerVisitor
      : virtual public TidyInterface::ActorVisitor {
   public:
    static const float kMinDepth;
    static const float kMaxDepth;

    explicit LayerVisitor(int32 count)
        : depth_(0.0f),
          layer_thickness_(0.0f),
          count_(count) {}
    virtual ~LayerVisitor() {}

    virtual void VisitActor(TidyInterface::Actor* actor);
    virtual void VisitStage(TidyInterface::StageActor* actor);
    virtual void VisitContainer(TidyInterface::ContainerActor* actor);
    virtual void VisitQuad(TidyInterface::QuadActor* actor);
    virtual void VisitTexturePixmap(TidyInterface::TexturePixmapActor* actor);

   private:
    float depth_;
    float layer_thickness_;
    int32 count_;

    DISALLOW_COPY_AND_ASSIGN(LayerVisitor);
  };

  class Actor : virtual public ClutterInterface::Actor,
                public TidyInterface::VisitorDestination {
   public:
    explicit Actor(TidyInterface* interface);
    virtual ~Actor();

    // Begin ClutterInterface::VisitorDestination methods
    virtual void Accept(ActorVisitor* visitor) {
      DCHECK(visitor);
      visitor->VisitActor(this);
    }
    // End ClutterInterface::VisitorDestination methods

    // Begin ClutterInterface::Actor methods
    virtual Actor* Clone();
    int GetWidth() { return width_; }
    int GetHeight() { return height_; }
    int GetX() { return x_; }
    int GetY() { return y_; }
    double GetXScale() { return scale_x_; }
    double GetYScale() { return scale_y_; }
    void SetVisibility(bool visible) {
      visible_ = visible;
      SetDirty();
    }
    void SetSize(int width, int height) {
      width_ = width;
      height_ = height;
      SetSizeImpl(&width_, &height_);
      SetDirty();
    }
    void SetName(const std::string& name) { name_ = name; }
    const std::string& name() const { return name_; }

    void Move(int x, int y, int duration_ms);
    void MoveX(int x, int duration_ms);
    void MoveY(int y, int duration_ms);
    void Scale(double scale_x, double scale_y, int duration_ms);
    void SetOpacity(double opacity, int duration_ms);
    void SetClip(int x, int y, int width, int height) { NOTIMPLEMENTED(); }

    void Raise(ClutterInterface::Actor* other);
    void Lower(ClutterInterface::Actor* other);
    void RaiseToTop();
    void LowerToBottom();
    virtual std::string GetDebugString(int indent_level) {
      return GetDebugStringInternal("Actor", indent_level);
    }
    // End ClutterInterface::Actor methods

    // Updates the actor in response to time passing, and counts the
    // number of actors as it goes.
    virtual void Update(int32* count, AnimationTime now);

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

    // Note that is_opaque isn't valid until after a LayerVisitor has
    // been run over the tree -- that's what calculates the opacity
    // flag.
    bool is_opaque() const { return is_opaque_; }

    bool IsVisible() const { return visible_ && opacity_ > 0.001f; }
    float opacity() const { return opacity_; }
    float scale_x() const { return scale_x_; }
    float scale_y() const { return scale_y_; }
    void SetDirty() { interface_->SetDirty(); }

    // Shows a horizontal gradient (transparent on left to black on
    // right) over the window's client area if true.  Does nothing if
    // false.  Defaults to false.
    void ShowDimmed(bool dimmed, int anim_ms);
    bool is_dimmed() const { return dimmed_opacity_ > 0.001f; }
    float dimmed_opacity() const { return dimmed_opacity_; }

    // Sets the drawing data of the given type on this object.
    void SetDrawingData(int32 id, DrawingDataPtr data) {
      drawing_data_[id] = data;
    }

    // Gets the drawing data of the given type.
    DrawingDataPtr GetDrawingData(int32 id) const;

    // Erases the drawing data of the given type.
    void EraseDrawingData(int32 id) { drawing_data_.erase(id); }

   protected:
    // So it can update the opacity flag.
    friend class TidyInterface::LayerVisitor;

    TidyInterface* interface() { return interface_; }

    void CloneImpl(Actor* clone);
    virtual void SetSizeImpl(int* width, int* height) {}

    // Helper method that can be invoked by derived classes.  Returns a
    // string defining this actor, saying that its type is 'type_name'
    // (e.g. "RectangleActor", "TexturePixmapActor", etc.).
    std::string GetDebugStringInternal(const std::string& type_name,
                                       int indent_level);

    void set_has_children(bool has_children) { has_children_ = has_children; }
    void set_is_opaque(bool opaque) { is_opaque_ = opaque; }

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

    TidyInterface* interface_;

    // This points to the parent that has this actor as a child.
    ContainerActor* parent_;

    // This is the x position on the screen.
    int x_;

    // This is the y position on the screen.
    int y_;

    // This is the width and height of the actor's bounding box.
    int width_;
    int height_;

    // This is the z depth of this actor (which is set according to
    // the layer this actor is on.
    float z_;

    // This is the x and y scale of the actor.
    float scale_x_;
    float scale_y_;

    // This is the opacity of the actor (0 = transparent, 1 = opaque)
    float opacity_;

    // Calculated during the layer visitor pass, and used to determine
    // if this object is opaque for traversal purposes.
    bool is_opaque_;

    // This indicates if this actor has any children (false for all
    // but containers).  This is here so we can avoid a virtual
    // function call to determine this during the drawing traversal.
    bool has_children_;

    // This says whether or not to show this actor.
    bool visible_;

    // The opacity of the dimming quad.
    float dimmed_opacity_;

    // This is a name used for identifying the actor (most useful for
    // debugging).
    std::string name_;

    // Map from the address of a field to the animation that is modifying it.
    std::map<int*, std::tr1::shared_ptr<Animation<int> > > int_animations_;
    std::map<float*, std::tr1::shared_ptr<Animation<float> > >
        float_animations_;

    // This keeps a mapping of int32 id to drawing data pointer.
    // The id space is maintained by the visitor implementation.
    DrawingDataMap drawing_data_;

    DISALLOW_COPY_AND_ASSIGN(Actor);
  };

  class ContainerActor : public TidyInterface::Actor,
                         virtual public ClutterInterface::ContainerActor {
   public:
    explicit ContainerActor(TidyInterface* interface)
        : TidyInterface::Actor(interface) {
    }
    ~ContainerActor();

    // Implement VisitorDestination for visitor.
    void Accept(ActorVisitor* visitor) {
      CHECK(visitor);
      visitor->VisitContainer(this);
    }

    // Begin ClutterInterface::Actor methods
    virtual Actor* Clone() {
      NOTIMPLEMENTED();
      CHECK(false);
      return NULL;
    }
    virtual std::string GetDebugString(int indent_level);
    // End ClutterInterface::Actor methods

    // Begin TidyInterface::Actor methods
    virtual ActorVector GetChildren() { return children_; }
    // End TidyInterface::Actor methods

    // Begin ClutterInterface::ContainerActor methods
    void AddActor(ClutterInterface::Actor* actor);
    // End ClutterInterface::ContainerActor methods

    void RemoveActor(ClutterInterface::Actor* actor);
    virtual void Update(int32* count, AnimationTime now);

    // Raise one child over another.  Raise to top if "above" is NULL.
    void RaiseChild(TidyInterface::Actor* child,
                    TidyInterface::Actor* above);
    // Lower one child under another.  Lower to bottom if "below" is NULL.
    void LowerChild(TidyInterface::Actor* child,
                    TidyInterface::Actor* below);

   protected:
    virtual void SetSizeImpl(int* width, int* height) {
      // For containers, the size is always 1x1.
      // TODO: Implement a more complete story for setting sizes of containers.
      *width = 1;
      *height = 1;
    }
   private:
    // The list of this container's children.
    ActorVector children_;
    DISALLOW_COPY_AND_ASSIGN(ContainerActor);
  };

  // This class represents a quadrilateral.
  class QuadActor : public TidyInterface::Actor {
   public:
    explicit QuadActor(TidyInterface* interface);

    // Begin ClutterInterface::Actor methods
    virtual std::string GetDebugString(int indent_level) {
      return GetDebugStringInternal("QuadActor", indent_level);
    }
    // End ClutterInterface::Actor methods

    void SetColor(const ClutterInterface::Color& color,
                  const ClutterInterface::Color& border_color,
                  int border_width) {
      DCHECK(border_width >= 0);
      color_ = color;
      border_color_ = color;
      border_width = border_width;
    }
    const ClutterInterface::Color& color() const { return color_; }
    const ClutterInterface::Color& border_color() const {
      return border_color_;
    }
    const int border_width() const { return border_width_; }

    // Implement VisitorDestination for visitor.
    void Accept(ActorVisitor* visitor) {
      CHECK(visitor);
      visitor->VisitQuad(this);
    }

    virtual Actor* Clone();

   protected:
    void CloneImpl(QuadActor* clone);

   private:
    ClutterInterface::Color color_;
    ClutterInterface::Color border_color_;
    int border_width_;

    DISALLOW_COPY_AND_ASSIGN(QuadActor);
  };

  class TexturePixmapActor : public TidyInterface::QuadActor,
                             public ClutterInterface::TexturePixmapActor {
   public:
    explicit TexturePixmapActor(TidyInterface* interface);
    virtual ~TexturePixmapActor() { Reset(); }

    XWindow texture_pixmap_window() const { return window_; }

    // Begin ClutterInterface::Actor methods
    virtual std::string GetDebugString(int indent_level) {
      return GetDebugStringInternal("TexturePixmapActor", indent_level);
    }
    // End ClutterInterface::Actor methods

    // Implement VisitorDestination for visitor.
    void Accept(ActorVisitor* visitor) {
      CHECK(visitor);
      visitor->VisitTexturePixmap(this);
    }

    // Begin ClutterInterface::TexturePixmapActor methods
    bool SetTexturePixmapWindow(XWindow xid);
    bool IsUsingTexturePixmapExtension() { return true; }
    bool SetAlphaMask(const unsigned char* bytes, int width, int height) {
      NOTIMPLEMENTED();
      return true;
    }
    void ClearAlphaMask() { NOTIMPLEMENTED(); }
    // End ClutterInterface::TexturePixmapActor methods

    // Refresh the current pixmap.
    void RefreshPixmap();

    // Stop monitoring the current window, if any, for changes and destroy
    // the current pixmap.
    void Reset();

    // Throw out the current pixmap.  A new one will be created
    // automatically when needed.
    void DestroyPixmap();

    virtual Actor* Clone();

   protected:
    void CloneImpl(TexturePixmapActor* clone);

   private:
    FRIEND_TEST(TidyTest, HandleXEvents);

    // Is there currently any pixmap drawing data?  Tests use this to
    // check that old pixmaps get thrown away when needed.
    bool HasPixmapDrawingData();

    // This is the XWindow that this actor is associated with.
    XWindow window_;

    DISALLOW_COPY_AND_ASSIGN(TexturePixmapActor);
  };

  class StageActor : public TidyInterface::ContainerActor,
                     public ClutterInterface::StageActor {
   public:
    StageActor(TidyInterface* interface, int width, int height);
    virtual ~StageActor();

    // Implement VisitorDestination for visitor.
    void Accept(ActorVisitor* visitor) {
      CHECK(visitor);
      visitor->VisitStage(this);
    }

    virtual Actor* Clone() {
      NOTIMPLEMENTED();
      return NULL;
    }

    XWindow GetStageXWindow() { return window_; }
    void SetStageColor(const ClutterInterface::Color& color);
    const ClutterInterface::Color& stage_color() const {
      return stage_color_;
    }

   protected:
    virtual void SetSizeImpl(int* width, int* height);

   private:
    // This is the XWindow associated with the stage.  Owned by this class.
    XWindow window_;

    ClutterInterface::Color stage_color_;
    DISALLOW_COPY_AND_ASSIGN(StageActor);
  };

  TidyInterface(EventLoop* event_loop,
                XConnection* x_conn,
                GLInterfaceBase* gl_interface);
  ~TidyInterface();

  // Begin ClutterInterface methods
  void SetEventSource(CompositorEventSource* source) { event_source_ = source; }
  ContainerActor* CreateGroup();
  Actor* CreateRectangle(const ClutterInterface::Color& color,
                         const ClutterInterface::Color& border_color,
                         int border_width);
  Actor* CreateImage(const std::string& filename);
  TexturePixmapActor* CreateTexturePixmap();
  Actor* CreateText(const std::string& font_name,
                    const std::string& text,
                    const ClutterInterface::Color& color);
  Actor* CloneActor(ClutterInterface::Actor* orig);
  StageActor* GetDefaultStage() { return default_stage_.get(); }
  void HandleWindowConfigured(XWindow xid);
  void HandleWindowDestroyed(XWindow xid);
  void HandleWindowDamaged(XWindow xid);
  // End ClutterInterface methods

  XConnection* x_conn() { return x_conn_; }
  int actor_count() { return actor_count_; }
  bool dirty() const { return dirty_; }
  void set_current_time_ms_for_testing(int64_t time_ms) {
    current_time_ms_for_testing_ = time_ms;
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

  // Run in-progress animations and redraw the scene if needed.  Disables
  // the draw timeout if there are no in-progress animations.
  void Draw();

 private:
  FRIEND_TEST(OpenGlVisitorTestTree, LayerDepth);  // sets actor count

  // Used by tests.
  void set_actor_count(int count) { actor_count_ = count; }

  // This is called when we start monitoring for changes, and sets up
  // redirection for the supplied window.
  void StartMonitoringWindowForChanges(XWindow xid, TexturePixmapActor* actor);

  // This is called when we stop monitoring for changes, and removes
  // the redirection for the supplied window.
  void StopMonitoringWindowForChanges(XWindow xid, TexturePixmapActor* actor);

  // Enable or disable the draw timeout.  Safe to call if it's already
  // enabled/disabled.
  void EnableDrawTimeout();
  void DisableDrawTimeout();

  EventLoop* event_loop_;  // not owned
  XConnection* x_conn_;    // not owned

  // The source that will be sending us X events related to windows used
  // for TexturePixmapActors (typically WindowManager).  We need to be able
  // to tell the source when we're interested or uninterested in receiving
  // events about a particular window.
  CompositorEventSource* event_source_;  // not owned and NULL if unset

  // This indicates if the interface is dirty and needs to be redrawn.
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

#ifdef TIDY_OPENGL
  OpenGlDrawVisitor* draw_visitor_;
#elif defined(TIDY_OPENGLES)
  OpenGlesDrawVisitor* draw_visitor_;
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

  DISALLOW_COPY_AND_ASSIGN(TidyInterface);
};

// Specialization for integer animations that rounds to the nearest position.
template<>
inline int TidyInterface::Animation<int>::InterpolateValue(
    int start_value, int end_value, float fraction) {
  return roundf(start_value + fraction * (end_value - start_value));
}

}  // namespace window_manager

#endif  // WINDOW_MANAGER_TIDY_INTERFACE_H_
