// This file Copyright © Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

#import <AppKit/AppKit.h>

#include <libtransmission/transmission.h>

typedef NS_ENUM(NSUInteger, InternetStateIndicatorState) {
    InternetStateIndicatorStateGreen,
    InternetStateIndicatorStateYellow,
    InternetStateIndicatorStateRed
};

@interface InternetStateIndicatorSnapshot : NSObject

@property(nonatomic, readonly) InternetStateIndicatorState state;
@property(nonatomic, readonly) NSString* toolTip;
@property(nonatomic, readonly) NSString* accessibilityLabel;

- (instancetype)initWithState:(InternetStateIndicatorState)state
                      toolTip:(NSString*)toolTip
           accessibilityLabel:(NSString*)accessibilityLabel NS_DESIGNATED_INITIALIZER;
+ (instancetype)snapshotWithState:(InternetStateIndicatorState)state
                          toolTip:(NSString*)toolTip
               accessibilityLabel:(NSString*)accessibilityLabel;

- (instancetype)init NS_UNAVAILABLE;

@end

@interface StatusBarController : NSTitlebarAccessoryViewController<NSMenuItemValidation>

- (instancetype)initWithLib:(tr_session*)lib;

- (void)updateWithDownload:(CGFloat)dlRate upload:(CGFloat)ulRate internetState:(InternetStateIndicatorSnapshot*)internetState;

- (void)updateSpeedFieldsToolTips;

@end
