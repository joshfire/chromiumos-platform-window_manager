// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "tree.h"
#include <ostream>
#include <string>

using std::ostream;
using std::string;
using std::endl;

//
// TreeNode definition
//
TreeNode::Frame::Frame()
    : count(0),
      total_time(0) {
}

TreeNode::TreeNode(const string& name)
    : name_(name) {
}

void TreeNode::AddChild(int id, TreeNode* child) {
  children_[id] = child;
}

TreeNode* TreeNode::GetChild(int id) {
  if (children_.find(id) == children_.end()) {
    return NULL;
  }
  return children_[id];
}

void TreeNode::Accept(int level, TreeVisitor* visitor) {
  visitor->Visit(level, this);
  for (Children::iterator it = children_.begin(); it != children_.end(); it++) {
    it->second->Accept(level + 1, visitor);
  }
}

//
// TreeVisitor definition
//
TreeVisitor::TreeVisitor(ostream& output)
    : output_(output),
      row_(0) {
}

void TreeVisitor::Visit(int level, TreeNode* node) {
  if (node->name().empty()) {
    return;
  }
  if (row_ == -2) {  // output tree hierarchy
    for (int i = 0; i < level; i++) {
      output_ << "+,";
    }
    output_ << "\"" << node->name() << "\"" << endl;
  } else if (row_ == -1) {  // output column headings
    output_ << ",\"" << node->name() << "\",";
  } else {
    TreeNode::Data& data = node->data();
    if (data.find(row_) == data.end()) {
      output_ << "-,-,";
    } else {
      output_ << data[row_].count << ","
              << data[row_].total_time << ",";
    }
  }
}

DetailTreeVisitor::DetailTreeVisitor(ostream& output)
    : TreeVisitor(output) {
}

void DetailTreeVisitor::Visit(int level, TreeNode* node) {
  // TODO: make a non-redundant version of the output file. The current format
  // is experimental.
  TreeNode::Data& data = node->data();
  for (int i = 0; i < level; i++) {
    output_ << "+,";
  }
  output_ << node->name();
  if (data.find(row_) == data.end()) {
    output_ << ",-,-";
  } else {
    output_ << "," << data[row_].count
            << "," << data[row_].total_time;
  }  // if
  output_ << endl;
}


