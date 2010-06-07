// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <map>
#include <ostream>
#include <string>

class TreeVisitor;

class TreeNode {
 public:
  struct Frame {
    int count;
    int64_t total_time;
    Frame();
  };

  typedef std::map<int, Frame> Data;

  TreeNode(const std::string& name);
  void AddChild(int id, TreeNode* child);
  TreeNode* GetChild(int id);
  void Accept(int level, TreeVisitor* visitor);

  const std::string& name() const { return name_; }
  Data& data() { return data_; }

 private:
  typedef std::map<int, TreeNode*> Children;

  std::string name_;
  Children children_;
  Data data_;
};

class TreeVisitor {
 public:
  TreeVisitor(std::ostream& output);
  virtual ~TreeVisitor() {}

  virtual void Visit(int level, TreeNode* node);

  void set_row(int row) {
    row_ = row;
  }
 protected:
  std::ostream& output_;
  int row_;
};

class DetailTreeVisitor : public TreeVisitor {
 public:
  DetailTreeVisitor(std::ostream& output);
  virtual void Visit(int level, TreeNode* node);
};
