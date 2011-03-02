// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINDOW_MANAGER_COMPOSITOR_COMPOSITOR_H_
#define WINDOW_MANAGER_COMPOSITOR_COMPOSITOR_H_

#include <string>
#include <tr1/unordered_set>

#include "base/basictypes.h"
#include "window_manager/compositor/animation.h"
#include "window_manager/geometry.h"
#include "window_manager/image_enums.h"
#include "window_manager/x11/x_types.h"

namespace window_manager {

class CompositionChangeListener;
class ImageContainer;
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

    // Color components in the range [0.0, 1.0].
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
    virtual Rect GetBounds() = 0;
    virtual int GetWidth() = 0;
    virtual int GetHeight() = 0;
    virtual int GetX() = 0;
    virtual int GetY() = 0;
    virtual double GetXScale() = 0;
    virtual double GetYScale() = 0;

    virtual void Move(int x, int y, int anim_ms) = 0;
    virtual void MoveX(int x, int anim_ms) = 0;
    virtual void MoveY(int y, int anim_ms) = 0;

    // Create and return a pair of animations for controlling the X and Y
    // components of the actor's position.  Ownership of the returned
    // AnimationPair is transferred to the caller, who is expected to add
    // additional keyframes to the animation and then pass ownership of the pair
    // back to the compositor via SetMoveAnimation().
    virtual AnimationPair* CreateMoveAnimation() = 0;

    // Assign a pair of animations previously allocated via this actor's
    // CreateMoveAnimation() method.  Takes ownership of |animations|.
    virtual void SetMoveAnimation(AnimationPair* animations) = 0;

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
    // successive value of |indent_level|.
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
    virtual void SetSize(int width, int height) = 0;
    virtual XWindow GetStageXWindow() = 0;
    virtual void SetStageColor(const Color &color) = 0;

   private:
    DISALLOW_COPY_AND_ASSIGN(StageActor);
  };

  // ColoredBoxActor displays a solid, colored rectangle.
  class ColoredBoxActor : virtual public Actor {
   public:
    ColoredBoxActor() {}
    virtual ~ColoredBoxActor() {}

    virtual void SetSize(int width, int height) = 0;
    virtual void SetColor(const Compositor::Color& color) = 0;

   private:
    DISALLOW_COPY_AND_ASSIGN(ColoredBoxActor);
  };

  // ImageActor displays a static image onscreen.
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
    // |bytes| must be of size |width| * |height|.
    virtual void SetAlphaMask(const uint8_t* bytes, int width, int height) = 0;

    // Clear the previously-applied alpha mask.
    virtual void ClearAlphaMask() = 0;

    // TODO(zmo@): merge this function with UpdateTexture.
    // Compute the union of the current damaged and the new region; this is
    // called at Damage event handler.
    virtual void MergeDamagedRegion(const Rect& region) = 0;

    // Get the currently damaged region; if (width, height) is (0, 0), then
    // the content is not dirty.
    virtual const Rect& GetDamagedRegion() const = 0;

    // Clear the previously set damaged region, i.e., set (width, height)
    // to (0, 0).
    virtual void ResetDamagedRegion() = 0;

   private:
    DISALLOW_COPY_AND_ASSIGN(TexturePixmapActor);
  };

  Compositor() : should_draw_frame_(true) {}
  virtual ~Compositor() {}

  bool should_draw_frame() const { return should_draw_frame_; }
  void set_should_draw_frame(bool should_draw_frame) {
    should_draw_frame_ = should_draw_frame;
  }

  virtual void RegisterCompositionChangeListener(
      CompositionChangeListener* listener) = 0;
  virtual void UnregisterCompositionChangeListener(
      CompositionChangeListener* listener) = 0;

  // Can we get windows' contents to the GPU without having to copy them to
  // userspace and then upload them to GL?
  virtual bool TexturePixmapActorUsesFastPath() = 0;

  // These methods create new Actor objects.  The caller is responsible for
  // deleting them, even after they have been added to a container.
  virtual ContainerActor* CreateGroup() = 0;
  virtual ColoredBoxActor* CreateColoredBox(
      int width, int height, const Compositor::Color& color) = 0;
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
  // visibility group included in |groups| (see
  // Actor::AddToVisibilityGroup()) will be hidden.  Passing an empty set
  // reverts to the standard behavior of drawing all actors that are
  // visible and at least partially opaque.
  virtual void SetActiveVisibilityGroups(
      const std::tr1::unordered_set<int>& groups) = 0;

  // Convenience methods to clear the current set of visibility groups (so
  // we display all actors) or display just a single group.
  void ResetActiveVisibilityGroups();
  void SetActiveVisibilityGroup(int group);

  // Draw the scene.  This happens automatically as needed but can also be
  // triggered manually.
  virtual void Draw() = 0;

 private:
  // This flag indicates whether the GL draw visitor should draw the frame
  // or it should skip the drawing.  The Draw() method is still invoked and
  // the LayerVisitor will still tranverse the tree, only the actual drawing
  // part is skipped.
  bool should_draw_frame_;

  DISALLOW_COPY_AND_ASSIGN(Compositor);
};

// Interface for classes that need to be notified when the composition
// of actors changes.
class CompositionChangeListener {
 public:
  // This method is called whenever the compositor notices a change in the
  // topmost visible fullscreen actor or a transition from having a fullscreen
  // actor on top to not having one (in which case top_fullscreen_actor is
  // NULL).  This is only called for TexturePixmapActor.
  virtual void HandleTopFullscreenActorChange(
      const Compositor::TexturePixmapActor* top_fullscreen_actor) = 0;

 protected:
  ~CompositionChangeListener() {}
};

}  // namespace window_manager

#endif  // WINDOW_MANAGER_COMPOSITOR_COMPOSITOR_H_
