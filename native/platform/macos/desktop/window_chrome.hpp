#pragma once

#import <Cocoa/Cocoa.h>

// Keep real AppKit window controls inside the same 51-point header as the web UI.
inline void loopzAlignWindowControls(NSWindow* window) {
    if (!window || (window.styleMask & NSWindowStyleMaskFullScreen))
        return;
    NSButton* close = [window standardWindowButton:NSWindowCloseButton];
    NSView* titlebar = close.superview;
    NSView* container = titlebar.superview;
    if (!close || !titlebar || !container)
        return;
    constexpr CGFloat headerHeight = 51.0;
    NSRect containerFrame = container.frame;
    containerFrame.origin.y = NSMaxY(containerFrame) - headerHeight;
    containerFrame.size.height = headerHeight;
    if (!NSEqualRects(container.frame, containerFrame))
        container.frame = containerFrame;
    NSRect titlebarFrame = titlebar.frame;
    titlebarFrame.size.height = headerHeight;
    if (!NSEqualRects(titlebar.frame, titlebarFrame))
        titlebar.frame = titlebarFrame;
    const NSWindowButton kinds[] = {NSWindowCloseButton, NSWindowMiniaturizeButton,
                                   NSWindowZoomButton};
    for (NSUInteger i = 0; i < 3; ++i) {
        NSButton* button = [window standardWindowButton:kinds[i]];
        if (!button)
            continue;
        NSRect frame = button.frame;
        frame.origin.x = 20.0 + 24.0 * i - frame.size.width / 2.0;
        frame.origin.y = (headerHeight - frame.size.height) / 2.0;
        if (!NSEqualRects(button.frame, frame))
            button.frame = frame;
    }
}
