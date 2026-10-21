#include "editing_actions.hpp"

NSMenu* loopzCreateEditingMenu(id<LZEditingActionTarget> target) {
    auto* menu = [[NSMenu alloc] initWithTitle:@"Edit"];
    auto* undo = [menu addItemWithTitle:@"Undo"
                                 action:@selector(undoRoutingOrText:)
                          keyEquivalent:@"z"];
    // Native Shift-Z events carry the shifted glyph in both character fields.
    auto* redo = [menu addItemWithTitle:@"Redo"
                                 action:@selector(redoRoutingOrText:)
                          keyEquivalent:@"Z"];
    undo.target = target;
    redo.target = target;
    redo.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagShift;
    [menu addItemWithTitle:@"Cut" action:@selector(cut:) keyEquivalent:@"x"];
    [menu addItemWithTitle:@"Copy" action:@selector(copy:) keyEquivalent:@"c"];
    [menu addItemWithTitle:@"Paste" action:@selector(paste:) keyEquivalent:@"v"];
    [menu addItemWithTitle:@"Select All" action:@selector(selectAll:) keyEquivalent:@"a"];
    return menu;
}

bool loopzCanPerformEditingAction(WKWebView* view, NSDictionary* context, bool redo) {
    if ([context[@"textEditing"] boolValue])
        return redo ? view.undoManager.canRedo : view.undoManager.canUndo;
    return [context[redo ? @"canRedo" : @"canUndo"] boolValue];
}

void loopzPerformEditingAction(WKWebView* view, bool redo) {
    NSString* focus =
        @"(() => { const t = document.activeElement; return !!(t && (t.isContentEditable || "
         "t.tagName === 'TEXTAREA' || (t.tagName === 'INPUT' && "
         "!['range','checkbox','radio','button'].includes(t.type)))); })()";
    [view evaluateJavaScript:focus
           completionHandler:^(id textEditing, NSError* error) {
               if (error || ![textEditing isKindOfClass:NSNumber.class])
                   return;
               if ([textEditing boolValue]) {
                   if (redo) {
                       if (view.undoManager.canRedo)
                           [view.undoManager redo];
                   } else if (view.undoManager.canUndo) {
                       [view.undoManager undo];
                   }
                   return;
               }
               NSString* event =
                   redo
                       ? @"window.dispatchEvent(new CustomEvent('loopz-history', {detail:'redo'}))"
                       : @"window.dispatchEvent(new CustomEvent('loopz-history', {detail:'undo'}))";
               [view evaluateJavaScript:event completionHandler:nil];
           }];
}
