// This file Copyright © Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

#import "InternetStateIndicatorView.h"

@implementation InternetStateIndicatorView

- (void)setIndicatorColor:(NSColor*)indicatorColor
{
    if ([self.indicatorColor isEqual:indicatorColor])
    {
        return;
    }

    _indicatorColor = indicatorColor;
    [self setNeedsDisplay:YES];
}

- (BOOL)isOpaque
{
    return NO;
}

- (void)drawRect:(NSRect)dirtyRect
{
    [super drawRect:dirtyRect];

    if (self.indicatorColor == nil)
    {
        return;
    }

    NSRect const indicatorRect = NSInsetRect(self.bounds, 1.0, 1.0);
    NSBezierPath* const path = [NSBezierPath bezierPathWithOvalInRect:indicatorRect];

    [self.indicatorColor setFill];
    [path fill];

    [[NSColor.separatorColor colorWithAlphaComponent:0.85] setStroke];
    [path stroke];
}

- (BOOL)accessibilityIsIgnored
{
    return NO;
}

- (NSString*)accessibilityRole
{
    return NSAccessibilityImageRole;
}

- (NSString*)accessibilityLabel
{
    return self.indicatorAccessibilityLabel ?: @"";
}

@end
