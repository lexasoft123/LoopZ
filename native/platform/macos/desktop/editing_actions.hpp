#pragma once

#import <WebKit/WebKit.h>

@protocol LZEditingActionTarget <NSObject>
- (void)undoRoutingOrText:(id)sender;
- (void)redoRoutingOrText:(id)sender;
@end

NSMenu* loopzCreateEditingMenu(id<LZEditingActionTarget> target);
bool loopzCanPerformEditingAction(WKWebView* view, NSDictionary* context, bool redo);
// Native menu actions preserve WebKit text undo when a text editor has focus;
// otherwise a fixed UI event uses the normal revision-fenced routing store.
void loopzPerformEditingAction(WKWebView* view, bool redo);
