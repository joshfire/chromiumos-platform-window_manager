// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINDOW_MANAGER_COMPOSITOR_H_
#define WINDOW_MANAGER_COMPOSITOR_H_

#include <list>
#include <map>
#include <set>
#include <string>
#include <tr1/unordered_set>

#include "base/basictypes.h"
#include "base/logging.h"
#include "base/scoped_ptr.h"
#include "window_manager/image_enums.h"
#include "window_manager/x_types.h"

namespace window_manager {

class CompositorEventSource;
class ImageContainer;
template<class T> class Stacker;  // from util.h
class XConnection;

// A interface used for compositing windows and textures onscreen.
class Compositor {
 public:
  struct Color {
    Color() : red(0.f), green(0.f), blue(0.f) {}
    Color(float r, float g, float b) : red(r), green(g), blue(b) {}
    explicit Color(const std::string& hex_str) { CHECK(SetHex(hex_str)); }

    void SetHsv(float hue, float saturation, float value);

    // Set the color using a hex string of the form "#341a8b" or "#3ab"
    // (the latter form is expanded to "#33aabb").  The leading '#' is
    // optional, and letters can be either uppercase or lowercase.
    // Returns false if the conversion failed.
    bool SetHex(const std::string& hex_str);

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

    virtual void SetSize(int width, int height) = 0;
    virtual void Move(int x, int y, int anim_ms) = 0;
    virtual void MoveX(int x, int anim_ms) = 0;
    virtual void MoveY(int y, int anim_ms) = 0;
    virtual void Scale(double scale_x, double scale_y, int anim_ms) = 0;
    virtual void SetOpacity(double opacity, int anim_ms) = 0;
    virtual void Show() = 0;
    virtual void Hide() = 0;

    // Tilt is the amount of perspective to show, from 0.0 to
    // 1.0.  For 1.0, the actor is collapsed to a line.  For 0.0, the
    // actor is purely orthographic (screen aligned).  This represents
    // a perspective rotation around the Y axis, centered on the left
    // side of the actor, from 0 to 90 degrees.
    virtual void SetTilt(double tilt, int anim_ms) = 0;
    virtual double GetTilt() const = 0;

    // The width of an actor if it were tilted by the given amount.
    static int GetTiltedWidth(int width, double tilt);

    // Move an actor directly above or below another sibling actor in the
    // stacking order, or to the top or bottom of all of its siblings.
    virtual void Raise(Actor* other) = 0;
    virtual void Lower(Actor* other) = 0;
    virtual void RaiseToTop() = 0;
    virtual void LowerToBottom() = 0;

    // Get a string that briefly describes this actor and everything under
    // it in the tree.  The string will be indented two spaces for each
    // successive value of 'indent_level'.
    virtual std::string GetDebugString(int indent_level) = 0;

    // Shows a horizontal gradient (transparent on left to black on
    // right) over the window's client area if true.  Does nothing if
    // false.  Defaults to false.
    virtual void ShowDimmed(bool dimmed, int anim_ms) = 0;

    // Add or remove the actor from a visibility group.
    // See Compositor::SetActiveVisibilityGroups() for details.
    virtual void AddToVisibilityGroup(int group_id) = 0;
    virtual void RemoveFromVisibilityGroup(int group_id) = 0;

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

  // ImageActor displays static image onscreen.
  class ImageActor : virtual public Actor {
   public:
    ImageActor() {}
    virtual ~ImageActor() {}

    // Make the actor display the passed-in image data.
    virtual void SetImageData(const ImageContainer& image_container) = 0;
   private:
    DISALLOW_COPY_AND_ASSIGN(ImageActor);
  };

  // TexturePixmapActor displays the contents of a pixmap onscreen.
  class TexturePixmapActor : virtual public Actor {
   public:
    TexturePixmapActor() {}
    virtual ~TexturePixmapActor() {}

    // Create a texture containing the contents of the passed-in pixmap.
    virtual void SetPixmap(XID pixmap) = 0;

    // Update the texture after the contents of the pixmap have changed.
    virtual void UpdateTexture() = 0;

    // Add an additional texture to mask out parts of the actor.
    // 'bytes' must be of size 'width' * 'height'.
    virtual void SetAlphaMask(const uint8_t* bytes, int width, int height) = 0;

    // Clear the previously-applied alpha mask.
    virtual void ClearAlphaMask() = 0;

   private:
    DISALLOW_COPY_AND_ASSIGN(TexturePixmapActor);
  };

  Compositor() {}
  virtual ~Compositor() {}

  // Can we get windows' contents to the GPU without having to copy them to
  // userspace and then upload them to GL?
  virtual bool TexturePixmapActorUsesFastPath() = 0;

  // These methods create new Actor objects.  The caller is responsible for
  // deleting them, even after they have been added to a container.
  virtual ContainerActor* CreateGroup() = 0;
  virtual Actor* CreateRectangle(const Color& color, const Color& border_color,
                                 int border_width) = 0;
  virtual ImageActor* CreateImage() = 0;
  // Convenience method that assigns an image loaded from disk to the actor.
  // TODO: This would ideally be implemented within this base class, but we
  // don't want tests to have to load images from disk.
  virtual ImageActor* CreateImageFromFile(const std::string& filename) = 0;
  virtual TexturePixmapActor* CreateTexturePixmap() = 0;
  virtual Actor* CloneActor(Actor* orig) = 0;

  // Get the default stage object.  Ownership of the StageActor remains
  // with Compositor -- the caller should not delete it.
  virtual StageActor* GetDefaultStage() = 0;

  // Limit which actors will be drawn.  Actors that aren't members of a
  // visibility group included in 'groups' (see
  // Actor::AddToVisibilityGroup()) will be hidden.  Passing an empty set
  // reverts to the standard behavior of drawing all actors that are
  // visible and at least partially opaque.
  virtual void SetActiveVisibilityGroups(
      const std::tr1::unordered_set<int>& groups) = 0;

  // Draw the scene.  This happens automatically as needed but can also be
  // triggered manually.
  virtual void Draw() = 0;

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
        : x_(0),
          y_(0),
          width_(1),
          height_(1),
          scale_x_(1.0),
          scale_y_(1.0),
          opacity_(1.0),
          is_dimmed_(false),
          is_shown_(true),
          num_moves_(0),
          position_was_animated_(false),
          parent_(NULL) {
    }
    virtual ~Actor();

    int x() const { return x_; }
    int y() const { return y_; }
    double scale_x() const { return scale_x_; }
    double scale_y() const { return scale_y_; }
    double opacity() const { return opacity_; }
    bool is_dimmed() const { return is_dimmed_; }
    bool is_shown() const { return is_shown_; }
    int num_moves() const { return num_moves_; }
    bool position_was_animated() const { return position_was_animated_; }
    const std::set<int>& visibility_groups() const {
      return visibility_groups_;
    }

    MockCompositor::ContainerActor* parent() { return parent_; }
    void set_parent(MockCompositor::ContainerActor* new_parent) {
      parent_ = new_parent;
    }

    // Begin Compositor::Actor methods.
    virtual void SetName(const std::string& name) { name_ = name; }
    virtual int GetWidth() { return width_; }
    virtual int GetHeight() { return height_; }
    virtual int GetX() { return x_; }
    virtual int GetY() { return y_; }
    virtual double GetXScale() { return scale_x_; }
    virtual double GetYScale() { return scale_y_; }
    virtual void SetSize(int width, int height) {
      width_ = width;
      height_ = height;
    }
    virtual void Move(int x, int y, int anim_ms) {
      x_ = x;
      y_ = y;
      num_moves_++;
      position_was_animated_ = (anim_ms > 0);
    }
    virtual void MoveX(int x, int anim_ms) { Move(x, y_, anim_ms); }
    virtual void MoveY(int y, int anim_ms) { Move(x_, y, anim_ms); }
    virtual void Scale(double scale_x, double scale_y, int anim_ms) {
      scale_x_ = scale_x;
      scale_y_ = scale_y;
    }
    virtual void SetOpacity(double opacity, int anim_ms) { opacity_ = opacity; }
    virtual void Show() { is_shown_ = true; }
    virtual void Hide() { is_shown_ = false; }
    virtual void SetTilt(double tilt, int anim_ms) { tilt_ = tilt; }
    virtual double GetTilt() const { return tilt_; }
    virtual void Raise(Compositor::Actor* other);
    virtual void Lower(Compositor::Actor* other);
    virtual void RaiseToTop();
    virtual void LowerToBottom();
    virtual std::string GetDebugString(int indent_level);
    virtual void ShowDimmed(bool dimmed, int anim_ms) { is_dimmed_ = dimmed; }
    virtual void AddToVisibilityGroup(int group_id) {
      visibility_groups_.insert(group_id);
    }
    virtual void RemoveFromVisibilityGroup(int group_id) {
      visibility_groups_.erase(group_id);
    }
    // End Compositor::Actor methods.

   protected:
    std::string name_;
    int x_, y_;
    int width_, height_;
    double scale_x_, scale_y_;
    double opacity_;
    double tilt_;
    bool is_dimmed_;
    bool is_shown_;

    // Number of times that the actor has been moved.
    int num_moves_;

    // Did the last call to Move() contain a non-zero duration?
    bool position_was_animated_;

    MockCompositor::ContainerActor* parent_;  // not owned

    std::set<int> visibility_groups_;

    DISALLOW_COPY_AND_ASSIGN(Actor);
  };

  class ContainerActor : public MockCompositor::Actor,
                         virtual public Compositor::ContainerActor {
   public:
    ContainerActor();
    virtual ~ContainerActor();

    // Begin Compositor::ContainerActor methods.
    void AddActor(Compositor::Actor* actor);
    // End Compositor::ContainerActor methods.

    // Begin Compositor::Actor methods.
    virtual std::string GetDebugString(int indent_level);
    // End Compositor::Actor methods.

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

    // Begin Compositor::StageActor methods.
    virtual XWindow GetStageXWindow() { return 0; }
    virtual void SetStageColor(const Compositor::Color& color) {}
    // End Compositor::StageActor methods.

   private:
    DISALLOW_COPY_AND_ASSIGN(StageActor);
  };

  class ImageActor : public MockCompositor::Actor,
                     public Compositor::ImageActor {
   public:
    ImageActor();
    virtual ~ImageActor() {}

    // Begin Compositor::Actor methods.
    virtual void SetSize(int width, int height) {}
    // End Compositor::Actor methods.

    // Begin Compositor::ImageActor methods.
    virtual void SetImageData(const ImageContainer& image_container);
    // End Compositor::ImageActor methods.
   private:
    DISALLOW_COPY_AND_ASSIGN(ImageActor);
  };

  class TexturePixmapActor : public MockCompositor::Actor,
                             public Compositor::TexturePixmapActor {
   public:
    explicit TexturePixmapActor(XConnection* xconn);
    virtual ~TexturePixmapActor();
    const uint8_t* alpha_mask_bytes() const { return alpha_mask_bytes_; }
    XID pixmap() const { return pixmap_; }
    int num_texture_updates() const { return num_texture_updates_; }

    // Begin Compositor::Actor methods.
    virtual void SetSize(int width, int height) {}
    // End Compositor::Actor methods.

    // Begin Compositor::TexturePixmapActor methods.
    virtual void SetPixmap(XID pixmap);
    virtual void UpdateTexture() { num_texture_updates_++; }
    virtual void SetAlphaMask(const uint8_t* bytes, int width, int height);
    virtual void ClearAlphaMask();
    // End Compositor::TexturePixmapActor methods.

   private:
    XConnection* xconn_;  // not owned

    // Shape as set by SetAlphaMask(), or NULL if the actor is unshaped.
    uint8_t* alpha_mask_bytes_;

    // Redirected window that we're tracking.
    XWindow redirected_window_;

    // Pixmap that we're displaying.
    XID pixmap_;

    // Number of times that UpdateTexture() has been called.
    int num_texture_updates_;

    DISALLOW_COPY_AND_ASSIGN(TexturePixmapActor);
  };

  explicit MockCompositor(XConnection* xconn)
      : xconn_(xconn),
        num_draws_(0) {}
  ~MockCompositor() {}

  // Begin Compositor methods
  virtual bool TexturePixmapActorUsesFastPath() { return true; }
  virtual ContainerActor* CreateGroup() { return new ContainerActor; }
  virtual Actor* CreateRectangle(const Compositor::Color& color,
                                 const Compositor::Color& border_color,
                                 int border_width) {
    return new Actor;
  }
  virtual ImageActor* CreateImage() { return new ImageActor; }
  // We always pretend like we successfully loaded a 1x1 image instead of
  // actually trying to open the file.
  virtual ImageActor* CreateImageFromFile(const std::string& filename);
  virtual TexturePixmapActor* CreateTexturePixmap() {
    return new TexturePixmapActor(xconn_);
  }
  Actor* CloneActor(Compositor::Actor* orig) { return new Actor; }
  StageActor* GetDefaultStage() { return &default_stage_; }
  virtual void SetActiveVisibilityGroups(
      const std::tr1::unordered_set<int>& groups) {
    active_visibility_groups_ = groups;
  }
  virtual void Draw() { num_draws_++; }
  // End Compositor methods

  const std::tr1::unordered_set<int>& active_visibility_groups() const {
    return active_visibility_groups_;
  }
  int num_draws() const { return num_draws_; }

 private:
  XConnection* xconn_;  // not owned
  StageActor default_stage_;
  std::tr1::unordered_set<int> active_visibility_groups_;
  int num_draws_;

  DISALLOW_COPY_AND_ASSIGN(MockCompositor);
};

}  // namespace window_manager

#endif  // WINDOW_MANAGER_COMPOSITOR_H_
