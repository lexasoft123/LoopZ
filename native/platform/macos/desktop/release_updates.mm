#include "release_updates.hpp"
#import <AppKit/AppKit.h>

namespace {
NSString* const releasePage = @"https://github.com/lexasoft123/LoopZ/releases/latest";
NSString* const releaseAPI = @"https://api.github.com/repos/lexasoft123/LoopZ/releases/latest";
NSString* const preference = @"AutomaticReleaseChecks";
BOOL stableVersion(NSString* value, NSArray<NSNumber*>** components) {
    if (![value isKindOfClass:NSString.class] || value.length > 64)
        return NO;
    if ([value hasPrefix:@"v"])
        value = [value substringFromIndex:1];
    NSRegularExpression* pattern = [NSRegularExpression
        regularExpressionWithPattern:@"^(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)$"
                             options:0
                               error:nil];
    if ([pattern numberOfMatchesInString:value options:0 range:NSMakeRange(0, value.length)] != 1)
        return NO;
    NSMutableArray* result = [NSMutableArray array];
    for (NSString* part in [value componentsSeparatedByString:@"."]) {
        if (part.length > 8)
            return NO;
        [result addObject:@(part.longLongValue)];
    }
    *components = result;
    return YES;
}
} // namespace

BOOL loopzNewerStableRelease(NSString* candidate, NSString* current) {
    NSArray<NSNumber*>*a = nil, *b = nil;
    if (!stableVersion(candidate, &a) || !stableVersion(current, &b))
        return NO;
    for (NSUInteger i = 0; i < 3; ++i)
        if (![a[i] isEqual:b[i]])
            return a[i].longLongValue > b[i].longLongValue;
    return NO;
}

@implementation LZReleaseUpdates {
    NSString* _state;
    NSString* _version;
    NSTimer* _timer;
    NSURLSession* _session;
}
+ (instancetype)shared {
    static LZReleaseUpdates* instance;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        instance = [[self alloc] init];
    });
    return instance;
}
- (instancetype)init {
    if ((self = [super init])) {
        [NSUserDefaults.standardUserDefaults registerDefaults:@{preference : @YES}];
        _state = @"idle";
        NSURLSessionConfiguration* config = NSURLSessionConfiguration.ephemeralSessionConfiguration;
        config.timeoutIntervalForRequest = 20;
        config.timeoutIntervalForResource = 30;
        _session = [NSURLSession sessionWithConfiguration:config];
        _timer = [NSTimer scheduledTimerWithTimeInterval:6 * 60 * 60
                                                 repeats:YES
                                                   block:^(NSTimer*) {
                                                       if ([NSUserDefaults.standardUserDefaults
                                                               boolForKey:preference])
                                                           [self check];
                                                   }];
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC),
                       dispatch_get_main_queue(), ^{
                           if ([NSUserDefaults.standardUserDefaults boolForKey:preference])
                               [self check];
                       });
    }
    return self;
}
- (NSDictionary*)snapshot {
    return @{
        @"ok" : @YES,
        @"state" : _state,
        @"version" : _version ?: @"",
        @"currentVersion" :
                [NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleShortVersionString"]
            ?: @"0.1.0",
        @"automatic" : @([NSUserDefaults.standardUserDefaults boolForKey:preference])
    };
}
- (void)setAutomatic:(BOOL)enabled {
    [NSUserDefaults.standardUserDefaults setBool:enabled forKey:preference];
}
- (BOOL)openRelease {
    // Release JSON never supplies a navigation target to the privileged bridge.
    return [NSWorkspace.sharedWorkspace openURL:[NSURL URLWithString:releasePage]];
}
- (void)check {
    if ([_state isEqual:@"checking"])
        return;
    _state = @"checking";
    NSMutableURLRequest* request =
        [NSMutableURLRequest requestWithURL:[NSURL URLWithString:releaseAPI]];
    [request setValue:@"application/vnd.github+json" forHTTPHeaderField:@"Accept"];
    [request setValue:@"LoopZ-release-check" forHTTPHeaderField:@"User-Agent"];
    [[_session dataTaskWithRequest:request
                 completionHandler:^(NSData* data, NSURLResponse* response, NSError* error) {
                     NSHTTPURLResponse* http =
                         [response isKindOfClass:NSHTTPURLResponse.class] ? (id)response : nil;
                     NSDictionary* release = nil;
                     if (!error && http.statusCode == 200 && data.length <= 1024 * 1024) {
                         id decoded = [NSJSONSerialization JSONObjectWithData:data
                                                                      options:0
                                                                        error:nil];
                         if ([decoded isKindOfClass:NSDictionary.class])
                             release = decoded;
                     }
                     dispatch_async(dispatch_get_main_queue(), ^{
                         _version = nil;
                         NSArray<NSNumber*>* parts = nil;
                         if (!release || ![release[@"draft"] isKindOfClass:NSNumber.class] ||
                             ![release[@"prerelease"] isKindOfClass:NSNumber.class] ||
                             [release[@"draft"] boolValue] || [release[@"prerelease"] boolValue] ||
                             !stableVersion(release[@"tag_name"], &parts)) {
                             _state = http.statusCode == 404 ? @"unpublished" : @"error";
                             return;
                         }
                         NSString* tag = release[@"tag_name"];
                         NSString* current = [self snapshot][@"currentVersion"];
                         _state = loopzNewerStableRelease(tag, current) ? @"available" : @"current";
                         _version = [tag hasPrefix:@"v"] ? [tag substringFromIndex:1] : tag;
                     });
                 }] resume];
}
@end
