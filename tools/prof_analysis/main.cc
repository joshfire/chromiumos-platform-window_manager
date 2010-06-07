// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stdio.h>
#include <cassert>
#include <cstring>
#include <iostream>
#include <ostream>
#include <stack>
#include "tree.h"
#include "profiler_data.h"  // part of window_manager

namespace profiler = window_manager::profiler;

struct Profile {
  profiler::Symbol* symbols;
  profiler::Sample* samples;
  int num_symbols;
  int num_samples;
};

bool LoadProfileFromFile(const char* filename, Profile* pf) {
  FILE* fp = fopen(filename, "rb");
  if (fp == NULL) {
    return false;
  }

  int result = 0;
  int max_num_symbols;
  result += fread(&max_num_symbols, sizeof(max_num_symbols), 1, fp);
  result += fread(&pf->num_symbols, sizeof(pf->num_symbols), 1, fp);
  result += fread(&pf->num_samples, sizeof(pf->num_samples), 1, fp);

  pf->symbols = new profiler::Symbol[pf->num_symbols];
  pf->samples = new profiler::Sample[pf->num_samples];
  if (pf->symbols == NULL || pf->samples == NULL) {
    return false;
  }

  result += fread(pf->symbols, sizeof(pf->symbols[0]), pf->num_symbols, fp);
  result += fseek(fp,
                  sizeof(pf->symbols[0]) * (max_num_symbols - pf->num_symbols),
                  SEEK_CUR);
  result += fread(pf->samples, sizeof(pf->samples[0]), pf->num_samples, fp);

  fclose(fp);

  if (result != 3 + pf->num_symbols + pf->num_samples) {
    return false;
  }
  return true;
}

int BuildTreeFromProfile(const Profile& pf, TreeNode* current) {
  using std::stack;
  stack<TreeNode*> tree_stack;
  stack<profiler::Sample*> data_stack;
  int frame = 0;

  for (int i = 0; i < pf.num_samples; i++) {
    const profiler::Sample& sample = pf.samples[i];
    const profiler::Symbol& symbol = pf.symbols[sample.symbol_id];

    switch (sample.flag) {
      case profiler::MARK_FLAG_BEGIN: {
        TreeNode* node = current->GetChild(sample.symbol_id);
        // The markers from frame to frame might be different, if we see a new
        // marker, add it to the tree.
        if (node == NULL) {
          node = new TreeNode(symbol.name);
          current->AddChild(sample.symbol_id, node);
        }
        tree_stack.push(current);
        data_stack.push(const_cast<profiler::Sample*>(&sample));

        current = node;
        break;
      }
      case profiler::MARK_FLAG_END: {
        // The user might start profiling somewhere in the middle, so an
        // end marker might appear before a start.
        if (data_stack.empty()) {
          continue;
        }

        profiler::Sample* start_sample = data_stack.top();
        data_stack.pop();
        assert(start_sample->symbol_id == sample.symbol_id);

        current->data()[frame].count += 1;
        current->data()[frame].total_time += sample.time - start_sample->time;

        current = tree_stack.top();
        tree_stack.pop();

        // Simple way to find frame boundary is to assume everything in a frame
        // are enclosed in the root marker.
        if (tree_stack.empty()) {
          frame++;
        }
        break;
      }
      case profiler::MARK_FLAG_TAP: {
        // TODO: implement
        break;
      }
      default: {
        assert(false);
      }
    }
  }

  return frame;
}

// Filename of the profile should be passed in argv[1].
int main(int argc, char** argv) {
  using namespace std;
  if (argc != 2 && (argc != 3 || strcmp(argv[2], "detail") != 0)) {
    cerr << "Usage: " << argv[0] << " profile-filename [detail]" << endl;
    return -1;
  }

  Profile pf;
  bool result = LoadProfileFromFile(argv[1], &pf);
  if (!result) {
    cerr << "Failed to load profile " << argv[1] << endl;
    return -1;
  }

  ostream& output = cout;
  output << "number of symbols: " << pf.num_symbols << endl;
  output << "number of samples: " << pf.num_samples << endl;

  TreeNode root("");
  int frame = BuildTreeFromProfile(pf, &root);
  TreeVisitor* visitor = NULL;

  if (argc == 3) {
    visitor = new DetailTreeVisitor(output);
  } else {
    visitor = new TreeVisitor(output);
  }

  // Indices -2 and -1 are special rows used to signal visitor to output other
  // information such as column headers.
  for (int i = -2; i < frame; i++) {
    visitor->set_row(i);
    root.Accept(0, visitor);
    output << endl;
  }

  delete visitor;

  return 0;
}
