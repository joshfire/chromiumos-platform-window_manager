// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINDOW_MANAGER_MOCK_COMPOSITOR_H_
#define WINDOW_MANAGER_MOCK_COMPOSITOR_H_

#include <set>

#include "base/logging.h"
#include "base/scoped_ptr.h"
#include "window_manager/compositor.h"

namespace window_manager {

template<class T> class Stacker;  // from util.h

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
    virtual void Move(int x, int y, int anim_ms);
    virtual void MoveX(int x, int anim_ms) { Move(x, y_, anim_ms); }
    virtual void MoveY(int y, int anim_ms) { Move(x_, y, anim_ms); }
    virtual AnimationPair* CreateMoveAnimation();
    virtual void SetMoveAnimation(AnimationPair* animations);
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
    virtual void SetSizeInternal(int width, int height) {
      width_ = width;
      height_ = height;
    }

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
    virtual void SetSize(int width, int height) {
      SetSizeInternal(width, height);
    }
    virtual XWindow GetStageXWindow() { return 0; }
    virtual void SetStageColor(const Compositor::Color& color) {}
    // End Compositor::StageActor methods.

   private:
    DISALLOW_COPY_AND_ASSIGN(StageActor);
  };

  class ColoredBoxActor : public MockCompositor::Actor,
                          public Compositor::ColoredBoxActor {
   public:
    ColoredBoxActor(int width, int height, const Compositor::Color& color);
    virtual ~ColoredBoxActor() {}

    // Begin Compositor::ColoredBoxActor methods.
    virtual void SetSize(int width, int height) {
      SetSizeInternal(width, height);
    }
    virtual void SetColor(const Compositor::Color& color) { color_ = color; }
    // End Compositor::ColoredBoxActor methods.

   private:
    Compositor::Color color_;

    DISALLOW_COPY_AND_ASSIGN(ColoredBoxActor);
  };

  class ImageActor : public MockCompositor::Actor,
                     public Compositor::ImageActor {
   public:
    ImageActor();
    virtual ~ImageActor() {}

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

    // Begin Compositor::TexturePixmapActor methods.
    virtual void SetPixmap(XID pixmap);
    virtual void UpdateTexture() { num_texture_updates_++; }
    virtual void SetAlphaMask(const uint8_t* bytes, int width, int height);
    virtual void ClearAlphaMask();
    virtual void MergeDamagedRegion(const Rect& region);
    virtual const Rect& GetDamagedRegion() const;
    virtual void ResetDamagedRegion();
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

    // Dirty region.
    Rect damaged_region_;

    DISALLOW_COPY_AND_ASSIGN(TexturePixmapActor);
  };

  explicit MockCompositor(XConnection* xconn)
      : xconn_(xconn),
        num_draws_(0) {}
  ~MockCompositor() {}

  // Begin Compositor methods
  virtual void RegisterCompositionChangeListener(
      CompositionChangeListener* listener) {}
  virtual void UnregisterCompositionChangeListener(
      CompositionChangeListener* listener) {}
  virtual bool TexturePixmapActorUsesFastPath() { return true; }
  virtual ContainerActor* CreateGroup() { return new ContainerActor; }
  virtual ColoredBoxActor* CreateColoredBox(int width, int height,
                                            const Compositor::Color& color) {
    return new ColoredBoxActor(width, height, color);
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

#endif  // WINDOW_MANAGER_MOCK_COMPOSITOR_H_
