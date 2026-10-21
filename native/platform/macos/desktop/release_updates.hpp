#pragma once
#import <Foundation/Foundation.h>

// macOS follows SingZ's check-and-download flow; installation stays explicit.
@interface LZReleaseUpdates : NSObject
+ (instancetype)shared;
- (NSDictionary*)snapshot;
- (void)check;
- (void)setAutomatic:(BOOL)enabled;
- (BOOL)openRelease;
@end

BOOL loopzNewerStableRelease(NSString* candidate, NSString* current);
