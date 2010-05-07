// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINDOW_MANAGER_COMPOSITOR_H_
#define WINDOW_MANAGER_COMPOSITOR_H_

#include <list>
#include <map>
#include <string>

#include "base/basictypes.h"
#include "base/scoped_ptr.h"
#include "window_manager/x_types.h"

namespace window_manager {

class CompositorEventSource;
template<class T> class Stacker;  // from util.h
class XConnection;

// A interface used for compositing windows and textures onscreen.
class Compositor {
 public:
  struct Color {
    Color() : red(0.f), green(0.f), blue(0.f) {}
    Color(float r, float g, float b) : red(r), green(g), blue(b) {}
    float red;
    float green;
    float blue;
  };

  // Abstract base class for actors, inherited from by both:
  // - more-specific abstract classes within Compositor that add
  //   additional virtual methods
  // - concrete Actor classes inside of implementations of Compositor
  //   that implement this class's methods
  class Actor {
   public:
    Actor() {}
    virtual ~Actor() {}

    virtual void SetName(const std::string& name) = 0;
    virtual int GetWidth() = 0;
    virtual int GetHeight() = 0;
    virtual int GetX() = 0;
    virtual int GetY() = 0;
    virtual double GetXScale() = 0;
    virtual double GetYScale() = 0;

    virtual void SetVisibility(bool visible) = 0;
    virtual void SetSize(int width, int height) = 0;
    virtual void Move(int x, int y, int anim_ms) = 0;
    virtual void MoveX(int x, int anim_ms) = 0;
    virtual void MoveY(int y, int anim_ms) = 0;
    virtual void Scale(double scale_x, double scale_y, int anim_ms) = 0;
    virtual void SetOpacity(double opacity, int anim_ms) = 0;
    // Tilt is the amount of perspective to show, from 0.0 to
    // 1.0.  For 1.0, the actor is collapsed to a line.  For 0.0, the
    // actor is purely orthographic (screen aligned).  This represents
    // a perspective rotation around the Y axis, centered on the left
    // side of the actor, from 0 to 90 degrees.
    virtual void SetTilt(double tilt, int anim_ms) = 0;
    virtual double GetTilt() const = 0;
    virtual void SetClip(int x, int y, int width, int height) = 0;

    // Move an actor directly above or below another actor in the stacking
    // order, or to the top or bottom of all of its siblings.
    virtual void Raise(Actor* other) = 0;
    virtual void Lower(Actor* other) = 0;
    virtual void RaiseToTop() = 0;
    virtual void LowerToBottom() = 0;

    // Get a string that briefly describes this actor and everything under
    // it in the tree.  The string will be indented two spaces for each
    // successive value of 'indent_level'.
    virtual std::string GetDebugString(int indent_level) = 0;

    virtual void ShowDimmed(bool dimmed, int anim_ms) = 0;
   private:
    DISALLOW_COPY_AND_ASSIGN(Actor);
  };

  class ContainerActor : virtual public Actor {
   public:
    ContainerActor() {}
    virtual ~ContainerActor() {}
    virtual void AddActor(Actor* actor) = 0;
   private:
    DISALLOW_COPY_AND_ASSIGN(ContainerActor);
  };

  class StageActor : virtual public ContainerActor {
   public:
    StageActor() {}
    virtual ~StageActor() {}
    virtual XWindow GetStageXWindow() = 0;
    virtual void SetStageColor(const Color &color) = 0;
   private:
    DISALLOW_COPY_AND_ASSIGN(StageActor);
  };

  class TexturePixmapActor : virtual public Actor {
   public:
    TexturePixmapActor() {}
    virtual ~TexturePixmapActor() {}
    virtual bool SetTexturePixmapWindow(XWindow xid) = 0;
    virtual bool IsUsingTexturePixmapExtension() = 0;

    // Update our copy of the window's contents in response to notification
    // that they have been modified.
    virtual void UpdateContents() = 0;

    // Add an additional texture to mask out parts of the actor.
    // 'bytes' must be of size 'width' * 'height'.
    virtual bool SetAlphaMask(
        const unsigned char* bytes, int width, int height) = 0;

    // Clear the previously-applied alpha mask.
    virtual void ClearAlphaMask() = 0;

   private:
    DISALLOW_COPY_AND_ASSIGN(TexturePixmapActor);
  };

  Compositor() {}
  virtual ~Compositor() {}

  // These methods create new Actor objects.  The caller is responsible for
  // deleting them, even after they have been added to a container.
  virtual ContainerActor* CreateGroup() = 0;
  virtual Actor* CreateRectangle(const Color& color, const Color& border_color,
                                 int border_width) = 0;
  virtual Actor* CreateImage(const std::string& filename) = 0;
  virtual TexturePixmapActor* CreateTexturePixmap() = 0;
  virtual Actor* CreateText(const std::string& font_name,
                            const std::string& text,
                            const Color& color) = 0;
  virtual Actor* CloneActor(Actor* orig) = 0;

  // Get the default stage object.  Ownership of the StageActor remains
  // with Compositor -- the caller should not delete it.
  virtual StageActor* GetDefaultStage() = 0;

 private:
  DISALLOW_COPY_AND_ASSIGN(Compositor);
};

// Mock implementation of Compositor that is used for testing.
class MockCompositor : public Compositor {
 public:
  class ContainerActor;

  class Actor : virtual public Compositor::Actor {
   public:
    Actor()
        : x_(-1),
          y_(-1),
          width_(-1),
          height_(-1),
          scale_x_(1.0),
          scale_y_(1.0),
          opacity_(1.0),
          visible_(true),
          is_dimmed_(false),
          num_moves_(0),
          parent_(NULL) {
    }
    virtual ~Actor();

    int x() const { return x_; }
    int y() const { return y_; }
    double scale_x() const { return scale_x_; }
    double scale_y() const { return scale_y_; }
    double opacity() const { return opacity_; }
    bool visible() const { return visible_; }
    bool is_dimmed() const { return is_dimmed_; }
    int num_moves() const { return num_moves_; }

    MockCompositor::ContainerActor* parent() { return parent_; }
    void set_parent(MockCompositor::ContainerActor* new_parent) {
      parent_ = new_parent;
    }

    // Begin Compositor::Actor methods
    void SetName(const std::string& name) {}
    int GetWidth() { return width_; }
    int GetHeight() { return height_; }
    int GetX() { return x_; }
    int GetY() { return y_; }
    double GetXScale() { return scale_x_; }
    double GetYScale() { return scale_y_; }
    void SetVisibility(bool visible) { visible_ = visible; }
    void SetSize(int width, int height) {
      width_ = width;
      height_ = height;
    }
    void Move(int x, int y, int anim_ms) {
      x_ = x;
      y_ = y;
      num_moves_++;
    }
    void MoveX(int x, int anim_ms) { Move(x, y_, anim_ms); }
    void MoveY(int y, int anim_ms) { Move(x_, y, anim_ms); }
    void Scale(double scale_x, double scale_y, int anim_ms) {
      scale_x_ = scale_x;
      scale_y_ = scale_y;
    }
    void SetOpacity(double opacity, int anim_ms) { opacity_ = opacity; }
    void SetTilt(double tilt, int anim_ms) {
      tilt_ = tilt;
    }
    double GetTilt() const { return tilt_; }
    void SetClip(int x, int y, int width, int height) {}
    void Raise(Compositor::Actor* other);
    void Lower(Compositor::Actor* other);
    void RaiseToTop();
    void LowerToBottom();
    virtual std::string GetDebugString(int debug_level) { return ""; }
    void ShowDimmed(bool dimmed, int anim_ms) { is_dimmed_ = dimmed; }
    // End Compositor::Actor methods

   protected:
    int x_, y_;
    int width_, height_;
    double scale_x_, scale_y_;
    double opacity_;
    double tilt_;
    bool visible_;
    bool is_dimmed_;

    // Number of times that the actor has been moved.
    int num_moves_;

    MockCompositor::ContainerActor* parent_;  // not owned

    DISALLOW_COPY_AND_ASSIGN(Actor);
  };

  class ContainerActor : public MockCompositor::Actor,
                         virtual public Compositor::ContainerActor {
   public:
    ContainerActor();
    virtual ~ContainerActor();
    void AddActor(Compositor::Actor* actor);

    Stacker<Actor*>* stacked_children() { return stacked_children_.get(); }

    // Get an index representing an actor's stacking position inside of
    // this container.  Objects stacked higher have lower indexes.
    // Convenient for testing.
    int GetStackingIndex(Compositor::Actor* actor);

   private:
    scoped_ptr<Stacker<Actor*> > stacked_children_;

    DISALLOW_COPY_AND_ASSIGN(ContainerActor);
  };

  class StageActor : public MockCompositor::ContainerActor,
                     public Compositor::StageActor {
   public:
    StageActor() {}
    virtual ~StageActor() {}
    XWindow GetStageXWindow() { return 0; }
    void SetStageColor(const Compositor::Color& color) {}
   private:
    DISALLOW_COPY_AND_ASSIGN(StageActor);
  };

  class TexturePixmapActor : public MockCompositor::Actor,
                             public Compositor::TexturePixmapActor {
   public:
    explicit TexturePixmapActor(XConnection* xconn)
        : xconn_(xconn),
          alpha_mask_bytes_(NULL),
          xid_(0) {}
    virtual ~TexturePixmapActor() {
      ClearAlphaMask();
    }
    const unsigned char* alpha_mask_bytes() const { return alpha_mask_bytes_; }
    const XWindow xid() const { return xid_; }

    bool SetTexturePixmapWindow(XWindow xid);
    bool IsUsingTexturePixmapExtension() { return false; }
    void UpdateContents() {}
    bool SetAlphaMask(const unsigned char* bytes, int width, int height);
    void ClearAlphaMask();

   private:
    XConnection* xconn_;  // not owned

    // Shape as set by SetAlphaMask(), or NULL if the actor is unshaped.
    unsigned char* alpha_mask_bytes_;

    // Redirected window that we're displaying.
    XWindow xid_;

    DISALLOW_COPY_AND_ASSIGN(TexturePixmapActor);
  };

  explicit MockCompositor(XConnection* xconn) : xconn_(xconn) {}
  ~MockCompositor() {}

  // Begin Compositor methods
  ContainerActor* CreateGroup() { return new ContainerActor; }
  Actor* CreateRectangle(const Compositor::Color& color,
                         const Compositor::Color& border_color,
                         int border_width) {
    return new Actor;
  }
  Actor* CreateImage(const std::string& filename) { return new Actor; }
  TexturePixmapActor* CreateTexturePixmap() {
    return new TexturePixmapActor(xconn_);
  }
  Actor* CreateText(const std::string& font_name,
                    const std::string& text,
                    const Compositor::Color& color) {
    return new Actor;
  }
  Actor* CloneActor(Compositor::Actor* orig) { return new Actor; }
  StageActor* GetDefaultStage() { return &default_stage_; }
  // End Compositor methods

 private:
  XConnection* xconn_;  // not owned
  StageActor default_stage_;

  DISALLOW_COPY_AND_ASSIGN(MockCompositor);
};

}  // namespace window_manager

#endif  // WINDOW_MANAGER_COMPOSITOR_H_
