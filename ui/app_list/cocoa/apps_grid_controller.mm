// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#import "ui/app_list/cocoa/apps_grid_controller.h"

#include "base/mac/foundation_util.h"
#include "base/strings/sys_string_conversions.h"
#include "skia/ext/skia_utils_mac.h"
#include "ui/app_list/app_list_constants.h"
#include "ui/app_list/app_list_item_model.h"
#include "ui/app_list/app_list_model.h"
#include "ui/app_list/app_list_model_observer.h"
#include "ui/app_list/app_list_view_delegate.h"
#include "ui/base/models/list_model_observer.h"
#include "ui/gfx/image/image_skia_util_mac.h"

namespace {

// OSX app list has hardcoded rows and columns for now.
const int kFixedRows = 4;
const int kFixedColumns = 4;

// Padding space in pixels for fixed layout.
const CGFloat kLeftRightPadding = 20;
const CGFloat kTopPadding = 1;

// Preferred tile size when showing in fixed layout.
const CGFloat kPreferredTileWidth = 88;
const CGFloat kPreferredTileHeight = 98;

}  // namespace

@interface AppsGridController ()

- (void)loadAndSetView;

// Update the model in full, and rebuild subviews.
- (void)modelUpdated;

// Bridged method for ui::ListModelObserver.
- (void)listItemsAdded:(size_t)start
                 count:(size_t)count;

- (void)onItemClicked:(id)sender;

- (NSButton*)selectedButton;

@end

@interface AppsGridViewItem : NSCollectionViewItem;
@end

@implementation AppsGridViewItem

- (void)setSelected:(BOOL)flag {
  if (flag) {
    [[base::mac::ObjCCastStrict<NSButton>([self view]) cell]
        setBackgroundColor:[NSColor lightGrayColor]];
  } else {
    [[base::mac::ObjCCastStrict<NSButton>([self view]) cell]
        setBackgroundColor:gfx::SkColorToCalibratedNSColor(
            app_list::kContentsBackgroundColor)];
  }
  [super setSelected:flag];
}

@end

namespace app_list {

class AppsGridDelegateBridge : public ui::ListModelObserver {
 public:
  AppsGridDelegateBridge(AppsGridController* parent) : parent_(parent) {}

 private:
  // Overridden from ui::ListModelObserver:
  virtual void ListItemsAdded(size_t start, size_t count) OVERRIDE {
    [parent_ listItemsAdded:start count:count];
  }
  virtual void ListItemsRemoved(size_t start, size_t count) OVERRIDE {}
  virtual void ListItemMoved(size_t index, size_t target_index) OVERRIDE {}
  virtual void ListItemsChanged(size_t start, size_t count) OVERRIDE {
    NOTREACHED();
  }

  AppsGridController* parent_;  // Weak, owns us.

  DISALLOW_COPY_AND_ASSIGN(AppsGridDelegateBridge);
};

}  // namespace app_list

@implementation AppsGridController

- (id)initWithViewDelegate:
    (scoped_ptr<app_list::AppListViewDelegate>)appListViewDelegate {
  if ((self = [super init])) {
    scoped_ptr<app_list::AppListModel> model(new app_list::AppListModel);
    delegate_.reset(appListViewDelegate.release());
    bridge_.reset(new app_list::AppsGridDelegateBridge(self));
    items_.reset([[NSMutableArray alloc] init]);
    if (delegate_)
      delegate_->SetModel(model.get());
    [self loadAndSetView];
    [self setModel:model.Pass()];
  }
  return self;
}

- (void)dealloc {
  if (model_)
    model_->apps()->RemoveObserver(bridge_.get());

  [super dealloc];
}

- (NSCollectionView*)collectionView {
  return base::mac::ObjCCastStrict<NSCollectionView>(
      [base::mac::ObjCCastStrict<NSScrollView>([self view]) documentView]);
}

- (app_list::AppListModel*)model {
  return model_.get();
}

- (app_list::AppListViewDelegate*)delegate {
  return delegate_.get();
}

- (void)setModel:(scoped_ptr<app_list::AppListModel>)model {
  if (model_)
    model_->apps()->RemoveObserver(bridge_.get());

  model_.reset(model.release());
  if (model_)
    model_->apps()->AddObserver(bridge_.get());

  [self modelUpdated];
}

- (void)activateSelection {
  [[self selectedButton] performClick:self];
}

- (void)loadAndSetView {
  const CGFloat kViewWidth = kFixedColumns * kPreferredTileWidth +
      2 * kLeftRightPadding;
  const CGFloat kViewHeight = kFixedRows * kPreferredTileHeight + kTopPadding;

  scoped_nsobject<NSButton> prototypeButton(
      [[NSButton alloc] initWithFrame:NSZeroRect]);
  [prototypeButton setImagePosition:NSImageAbove];
  [prototypeButton setButtonType:NSMomentaryPushInButton];
  [prototypeButton setTarget:self];
  [prototypeButton setAction:@selector(onItemClicked:)];
  [prototypeButton setBordered:NO];

  scoped_nsobject<AppsGridViewItem> prototype([[AppsGridViewItem alloc] init]);
  [prototype setView:prototypeButton];

  NSSize itemSize = NSMakeSize(kPreferredTileWidth, kPreferredTileHeight);
  scoped_nsobject<NSCollectionView> tmpCollectionView(
      [[NSCollectionView alloc] init]);
  [tmpCollectionView setMaxNumberOfRows:kFixedRows];
  [tmpCollectionView setMinItemSize:itemSize];
  [tmpCollectionView setMaxItemSize:itemSize];
  [tmpCollectionView setItemPrototype:prototype];
  [tmpCollectionView setSelectable:YES];

  NSRect scrollFrame = NSMakeRect(0, 0, kViewWidth, kViewHeight);
  scoped_nsobject<NSScrollView> scrollView(
      [[NSScrollView alloc] initWithFrame:scrollFrame]);
  [scrollView setBorderType:NSNoBorder];
  [scrollView setLineScroll:kViewWidth];
  [scrollView setPageScroll:kViewWidth];
  [scrollView setScrollsDynamically:NO];
  [scrollView setDocumentView:tmpCollectionView];

  [self setView:scrollView];
}

- (void)onItemClicked:(id)sender {
  app_list::AppListItemModel* itemModel =
      static_cast<app_list::AppListItemModel*>(
          [[[sender cell] representedObject] pointerValue]);
  delegate_->ActivateAppListItem(itemModel, 0);
}

- (void)modelUpdated {
  [items_ removeAllObjects];
  [[self collectionView] setContent:items_];
  if (model_ && model_->apps()->item_count())
    [self listItemsAdded:0
                   count:model_->apps()->item_count()];
}

- (NSButton*)selectedButton {
  NSIndexSet* selection = [[self collectionView] selectionIndexes];
  if ([selection count]) {
    NSCollectionViewItem* item =
        [[self collectionView] itemAtIndex:[selection firstIndex]];
    return base::mac::ObjCCastStrict<NSButton>([item view]);
  }
  return nil;
}

- (void)listItemsAdded:(size_t)start
                 count:(size_t)count {
  for (size_t i = start; i < start + count; ++i)
    [items_ insertObject:[NSNumber numberWithInt:i] atIndex:i];

  [[self collectionView] setContent:items_];

  for (size_t i = start; i < start + count; ++i) {
    app_list::AppListItemModel* submodel = model_->apps()->GetItemAt(i);
    NSCollectionViewItem* item = [[self collectionView] itemAtIndex:i];
    NSButton* button = base::mac::ObjCCastStrict<NSButton>([item view]);
    [button setTitle:base::SysUTF8ToNSString(submodel->title().c_str())];
    [button setImage:gfx::NSImageFromImageSkia(submodel->icon())];
    [[button cell] setRepresentedObject:[NSValue valueWithPointer:submodel]];
  }
}

@end
