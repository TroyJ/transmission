// This file Copyright © Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

#import "InfoOptionsViewController.h"
#import "NSStringAdditions.h"
#import "Torrent.h"
#import "Utils.h"

typedef NS_ENUM(NSInteger, OptionPopupType) {
    OptionPopupTypeGlobal = 0,
    OptionPopupTypeNoLimit = 1,
    OptionPopupTypeLimit = 2,
};

typedef NS_ENUM(NSUInteger, OptionPopupPriority) {
    OptionPopupPriorityHigh = 0,
    OptionPopupPriorityNormal = 1,
    OptionPopupPriorityLow = 2,
};

typedef NS_ENUM(NSUInteger, RelocationActionType) {
    RelocationActionTypeNone = 0,
    RelocationActionTypeRetry = 1,
    RelocationActionTypeCancel = 2,
    RelocationActionTypeResume = 3,
};

static NSInteger const kInvalidValue = -99;

static CGFloat const kStackViewInset = 12.0;
static CGFloat const kStackViewSpacing = 8.0;
static CGFloat const kRelocationButtonSpacing = 6.0;

@interface InfoOptionsViewController ()

@property(nonatomic, copy) NSArray<Torrent*>* fTorrents;

@property(nonatomic) BOOL fSet;

@property(nonatomic) IBOutlet NSPopUpButton* fPriorityPopUp;
@property(nonatomic) IBOutlet NSPopUpButton* fRatioPopUp;
@property(nonatomic) IBOutlet NSPopUpButton* fIdlePopUp;
@property(nonatomic) IBOutlet NSButton* fUploadLimitCheck;
@property(nonatomic) IBOutlet NSButton* fDownloadLimitCheck;
@property(nonatomic) IBOutlet NSButton* fGlobalLimitCheck;
@property(nonatomic) IBOutlet NSButton* fRemoveSeedingCompleteCheck;
@property(nonatomic) IBOutlet NSTextField* fUploadLimitField;
@property(nonatomic) IBOutlet NSTextField* fDownloadLimitField;
@property(nonatomic) IBOutlet NSTextField* fRatioLimitField;
@property(nonatomic) IBOutlet NSTextField* fIdleLimitField;
@property(nonatomic) IBOutlet NSTextField* fUploadLimitLabel;
@property(nonatomic) IBOutlet NSTextField* fDownloadLimitLabel;
@property(nonatomic) IBOutlet NSTextField* fIdleLimitLabel;
@property(nonatomic) IBOutlet NSTextField* fRatioLimitGlobalLabel;
@property(nonatomic) IBOutlet NSTextField* fIdleLimitGlobalLabel;
@property(nonatomic) IBOutlet NSTextField* fPeersConnectLabel;
@property(nonatomic) IBOutlet NSTextField* fPeersConnectField;

@property(nonatomic, copy) NSString* fInitialString;

@property(nonatomic) IBOutlet NSStackView* fOptionsStackView;
@property(nonatomic) IBOutlet NSView* fSeedingView;
@property(nonatomic, readonly) CGFloat fHeightChange;
@property(nonatomic, readwrite) CGFloat fCurrentHeight;
@property(nonatomic, readonly) CGFloat fHorizLayoutHeight;
@property(nonatomic, readonly) CGFloat fHorizLayoutWidth;
@property(nonatomic, readonly) CGFloat fVertLayoutHeight;
@property(nonatomic) NSButton* fRelocationActionButton;
@property(nonatomic) NSLayoutConstraint* fSeedingViewHeightConstraint;
@property(nonatomic) CGFloat fSeedingViewBaseHeight;
@property(nonatomic) RelocationActionType fRelocationActionType;

@end

@implementation InfoOptionsViewController

- (instancetype)init
{
    if ((self = [super initWithNibName:@"InfoOptionsView" bundle:nil]))
    {
        self.title = NSLocalizedString(@"Options", "Inspector view -> title");
    }

    return self;
}

- (void)awakeFromNib
{
    [super awakeFromNib];
    [self checkWindowSize];

    self.fRelocationActionButton = [NSButton buttonWithTitle:@"" target:self action:@selector(handleRelocationAction:)];
    self.fRelocationActionButton.bezelStyle = NSBezelStyleRounded;
    self.fRelocationActionButton.controlSize = NSControlSizeSmall;
    self.fRelocationActionButton.hidden = YES;
    self.fRelocationActionButton.translatesAutoresizingMaskIntoConstraints = NO;
    [self.fSeedingView addSubview:self.fRelocationActionButton];
    [NSLayoutConstraint activateConstraints:@[
        [self.fRelocationActionButton.leadingAnchor constraintEqualToAnchor:self.fRemoveSeedingCompleteCheck.leadingAnchor],
        // under everything else in this column, not just under the checkbox it
        // is aligned with -- the Advanced rows sit below that checkbox
        [self.fRelocationActionButton.topAnchor constraintEqualToAnchor:self.fPeersConnectField.bottomAnchor
                                                               constant:kRelocationButtonSpacing],
        [self.fRelocationActionButton.topAnchor constraintGreaterThanOrEqualToAnchor:self.fRemoveSeedingCompleteCheck.bottomAnchor
                                                                            constant:kRelocationButtonSpacing],
        [self.fRelocationActionButton.trailingAnchor constraintLessThanOrEqualToAnchor:self.fSeedingView.trailingAnchor constant:-12.0],
    ]];

    // The seeding view has a fixed height in the xib and clips its contents, so
    // a button added under the last row is invisible until that height makes
    // room for it. Keep the constraint so it can be grown while the button is up.
    // The xib also pins the view's bottom to its last row (the peers field), so
    // growing the height alone is unsatisfiable and Auto Layout breaks the
    // growth -- which is why the button never showed (live re-check
    // 2026-08-26). The fixed height already sizes the column; drop the pin.
    for (NSLayoutConstraint* constraint in self.fSeedingView.constraints)
    {
        if (constraint.firstAttribute == NSLayoutAttributeHeight && constraint.secondItem == nil && constraint.firstItem == self.fSeedingView)
        {
            self.fSeedingViewHeightConstraint = constraint;
            self.fSeedingViewBaseHeight = constraint.constant;
        }
        else if (constraint.firstAttribute == NSLayoutAttributeBottom && constraint.firstItem == self.fSeedingView && constraint.secondItem == self.fPeersConnectField)
        {
            constraint.active = NO;
        }
    }

    [self setGlobalLabels];

    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(setGlobalLabels) name:@"UpdateGlobalOptions" object:nil];
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(updateOptionsNotification:)
                                               name:@"UpdateOptionsNotification"
                                             object:nil];
}

- (CGFloat)fHorizLayoutHeight
{
    // side by side, so the taller column sets the height -- the seeding column
    // is the taller one whenever the relocation action button is showing
    return MAX(NSHeight(self.fPriorityView.frame), NSHeight(self.fSeedingView.frame)) + 2 * kStackViewInset;
}

- (CGFloat)fHorizLayoutWidth
{
    return NSWidth(self.fPriorityView.frame) + NSWidth(self.fSeedingView.frame) + (2 * kStackViewInset) + kStackViewSpacing;
}

- (CGFloat)fVertLayoutHeight
{
    return NSHeight(self.fPriorityView.frame) + NSHeight(self.fSeedingView.frame) + (2 * kStackViewInset) + kStackViewSpacing;
}

- (CGFloat)fHeightChange
{
    return self.oldHeight - self.fCurrentHeight;
}

- (NSRect)viewRect
{
    NSRect viewRect = self.view.frame;

    CGFloat difference = self.fHeightChange;

    // we check for existence of self.view.window
    // as when view is shown from TorrentTableView.mm popover we don't want to customize the view height
    if (self.view.window)
    {
        viewRect.size.height -= difference;
    }

    return viewRect;
}

- (void)checkLayout
{
    if (NSWidth(self.view.window.frame) >= self.fHorizLayoutWidth + 1)
    {
        self.fOptionsStackView.orientation = NSUserInterfaceLayoutOrientationHorizontal;
        self.fCurrentHeight = self.fHorizLayoutHeight;
    }
    else
    {
        self.fOptionsStackView.orientation = NSUserInterfaceLayoutOrientationVertical;
        self.fCurrentHeight = self.fVertLayoutHeight;
    }
}

- (void)checkWindowSize
{
    self.oldHeight = self.fCurrentHeight;

    [self updateWindowLayout];
}

- (void)updateWindowLayout
{
    // we check for existence of self.view.window
    // as when view is shown from TorrentTableView.mm popover we don't want to customize the view height
    if (self.view.window)
    {
        [self checkLayout];

        CGFloat difference = self.fHeightChange;

        NSRect windowRect = self.view.window.frame;
        windowRect.origin.y += difference;
        windowRect.size.height -= difference;

        self.view.window.minSize = NSMakeSize(self.view.window.minSize.width, NSHeight(windowRect));
        self.view.window.maxSize = NSMakeSize(FLT_MAX, NSHeight(windowRect));

        self.view.frame = [self viewRect];
        [self.view.window setFrame:windowRect display:YES animate:YES];
    }
    else
    {
        // set popover width
        NSRect rect = self.view.frame;
        rect.size.width = NSWidth(self.fOptionsStackView.frame) + (2 * kStackViewInset);
        self.view.frame = rect;
    }
}

- (void)setInfoForTorrents:(NSArray<Torrent*>*)torrents
{
    //don't check if it's the same in case the metadata changed
    self.fTorrents = torrents;

    self.fSet = NO;
}

- (void)updateInfo
{
    if (!self.fSet)
    {
        [self setupInfo];
    }

    self.fSet = YES;
}

- (void)updateOptions
{
    if (self.fTorrents.count == 0)
    {
        [self updateRelocationActionControls];
        return;
    }

    //get bandwidth info
    NSEnumerator* enumerator = [self.fTorrents objectEnumerator];
    Torrent* torrent = [enumerator nextObject]; //first torrent

    NSInteger uploadUseSpeedLimit = [torrent usesSpeedLimit:YES] ? NSControlStateValueOn : NSControlStateValueOff;
    NSUInteger uploadSpeedLimit = [torrent speedLimit:YES];
    BOOL multipleUploadSpeedLimits = NO;
    NSInteger downloadUseSpeedLimit = [torrent usesSpeedLimit:NO] ? NSControlStateValueOn : NSControlStateValueOff;
    NSUInteger downloadSpeedLimit = [torrent speedLimit:NO];
    BOOL multipleDownloadSpeedLimits = NO;
    NSInteger globalUseSpeedLimit = torrent.usesGlobalSpeedLimit ? NSControlStateValueOn : NSControlStateValueOff;

    while ((torrent = [enumerator nextObject]) &&
           (uploadUseSpeedLimit != NSControlStateValueMixed || !multipleUploadSpeedLimits || downloadUseSpeedLimit != NSControlStateValueMixed ||
            !multipleDownloadSpeedLimits || globalUseSpeedLimit != NSControlStateValueMixed))
    {
        if (uploadUseSpeedLimit != NSControlStateValueMixed &&
            uploadUseSpeedLimit != ([torrent usesSpeedLimit:YES] ? NSControlStateValueOn : NSControlStateValueOff))
        {
            uploadUseSpeedLimit = NSControlStateValueMixed;
        }

        if (!multipleUploadSpeedLimits && uploadSpeedLimit != [torrent speedLimit:YES])
        {
            multipleUploadSpeedLimits = YES;
        }

        if (downloadUseSpeedLimit != NSControlStateValueMixed &&
            downloadUseSpeedLimit != ([torrent usesSpeedLimit:NO] ? NSControlStateValueOn : NSControlStateValueOff))
        {
            downloadUseSpeedLimit = NSControlStateValueMixed;
        }

        if (!multipleDownloadSpeedLimits && downloadSpeedLimit != [torrent speedLimit:NO])
        {
            multipleDownloadSpeedLimits = YES;
        }

        if (globalUseSpeedLimit != NSControlStateValueMixed &&
            globalUseSpeedLimit != (torrent.usesGlobalSpeedLimit ? NSControlStateValueOn : NSControlStateValueOff))
        {
            globalUseSpeedLimit = NSControlStateValueMixed;
        }
    }

    //set upload view
    self.fUploadLimitCheck.state = uploadUseSpeedLimit;
    self.fUploadLimitCheck.enabled = YES;

    self.fUploadLimitLabel.enabled = uploadUseSpeedLimit == NSControlStateValueOn;
    self.fUploadLimitField.enabled = uploadUseSpeedLimit == NSControlStateValueOn;
    if (!multipleUploadSpeedLimits)
    {
        self.fUploadLimitField.integerValue = uploadSpeedLimit;
    }
    else
    {
        self.fUploadLimitField.stringValue = @"";
    }

    //set download view
    self.fDownloadLimitCheck.state = downloadUseSpeedLimit;
    self.fDownloadLimitCheck.enabled = YES;

    self.fDownloadLimitLabel.enabled = downloadUseSpeedLimit == NSControlStateValueOn;
    self.fDownloadLimitField.enabled = downloadUseSpeedLimit == NSControlStateValueOn;
    if (!multipleDownloadSpeedLimits)
    {
        self.fDownloadLimitField.integerValue = downloadSpeedLimit;
    }
    else
    {
        self.fDownloadLimitField.stringValue = @"";
    }

    //set global check
    self.fGlobalLimitCheck.state = globalUseSpeedLimit;
    self.fGlobalLimitCheck.enabled = YES;

    //get ratio and idle info
    enumerator = [self.fTorrents objectEnumerator];
    torrent = [enumerator nextObject]; //first torrent

    NSInteger checkRatio = torrent.ratioSetting;
    NSInteger checkIdle = torrent.idleSetting;
    NSInteger removeWhenFinishSeeding = torrent.removeWhenFinishSeeding ? NSControlStateValueOn : NSControlStateValueOff;
    CGFloat ratioLimit = torrent.ratioLimit;
    BOOL multipleRatioLimits = NO;
    NSUInteger idleLimit = torrent.idleLimitMinutes;
    BOOL multipleIdleLimits = NO;

    while ((torrent = [enumerator nextObject]) &&
           (checkRatio != kInvalidValue || !multipleRatioLimits || checkIdle != kInvalidValue || !multipleIdleLimits))
    {
        if (checkRatio != kInvalidValue && checkRatio != torrent.ratioSetting)
        {
            checkRatio = kInvalidValue;
        }

        if (!multipleRatioLimits && !isRatioEqual(ratioLimit, torrent.ratioLimit))
        {
            multipleRatioLimits = YES;
        }

        if (checkIdle != kInvalidValue && checkIdle != torrent.idleSetting)
        {
            checkIdle = kInvalidValue;
        }

        if (!multipleIdleLimits && idleLimit != torrent.idleLimitMinutes)
        {
            multipleIdleLimits = YES;
        }

        if (removeWhenFinishSeeding != NSControlStateValueMixed &&
            removeWhenFinishSeeding != (torrent.removeWhenFinishSeeding ? NSControlStateValueOn : NSControlStateValueOff))
        {
            removeWhenFinishSeeding = NSControlStateValueMixed;
        }
    }

    //set ratio view
    NSInteger index;
    if (checkRatio == TR_RATIOLIMIT_SINGLE)
    {
        index = OptionPopupTypeLimit;
    }
    else if (checkRatio == TR_RATIOLIMIT_UNLIMITED)
    {
        index = OptionPopupTypeNoLimit;
    }
    else if (checkRatio == TR_RATIOLIMIT_GLOBAL)
    {
        index = OptionPopupTypeGlobal;
    }
    else
    {
        index = -1;
    }
    [self.fRatioPopUp selectItemAtIndex:index];
    self.fRatioPopUp.enabled = YES;

    self.fRatioLimitField.hidden = checkRatio != TR_RATIOLIMIT_SINGLE;
    if (!multipleRatioLimits)
    {
        self.fRatioLimitField.floatValue = ratioLimit;
    }
    else
    {
        self.fRatioLimitField.stringValue = @"";
    }

    self.fRatioLimitGlobalLabel.hidden = checkRatio != TR_RATIOLIMIT_GLOBAL;

    //set idle view
    if (checkIdle == TR_IDLELIMIT_SINGLE)
    {
        index = OptionPopupTypeLimit;
    }
    else if (checkIdle == TR_IDLELIMIT_UNLIMITED)
    {
        index = OptionPopupTypeNoLimit;
    }
    else if (checkIdle == TR_IDLELIMIT_GLOBAL)
    {
        index = OptionPopupTypeGlobal;
    }
    else
    {
        index = -1;
    }
    [self.fIdlePopUp selectItemAtIndex:index];
    self.fIdlePopUp.enabled = YES;

    self.fIdleLimitField.hidden = checkIdle != TR_IDLELIMIT_SINGLE;
    if (!multipleIdleLimits)
    {
        self.fIdleLimitField.integerValue = idleLimit;
    }
    else
    {
        self.fIdleLimitField.stringValue = @"";
    }
    self.fIdleLimitLabel.hidden = checkIdle != TR_IDLELIMIT_SINGLE;

    self.fIdleLimitGlobalLabel.hidden = checkIdle != TR_IDLELIMIT_GLOBAL;

    //set remove transfer when seeding finishes
    self.fRemoveSeedingCompleteCheck.state = removeWhenFinishSeeding;
    self.fRemoveSeedingCompleteCheck.enabled = YES;

    //get priority info
    enumerator = [self.fTorrents objectEnumerator];
    torrent = [enumerator nextObject]; //first torrent

    NSInteger priority = torrent.priority;

    while ((torrent = [enumerator nextObject]) && priority != kInvalidValue)
    {
        if (priority != torrent.priority)
        {
            priority = kInvalidValue;
        }
    }

    //set priority view
    if (priority == TR_PRI_HIGH)
    {
        index = OptionPopupPriorityHigh;
    }
    else if (priority == TR_PRI_NORMAL)
    {
        index = OptionPopupPriorityNormal;
    }
    else if (priority == TR_PRI_LOW)
    {
        index = OptionPopupPriorityLow;
    }
    else
    {
        index = -1;
    }
    [self.fPriorityPopUp selectItemAtIndex:index];
    self.fPriorityPopUp.enabled = YES;

    //get peer info
    enumerator = [self.fTorrents objectEnumerator];
    torrent = [enumerator nextObject]; //first torrent

    NSInteger maxPeers = torrent.maxPeerConnect;

    while ((torrent = [enumerator nextObject]))
    {
        if (maxPeers != torrent.maxPeerConnect)
        {
            maxPeers = kInvalidValue;
            break;
        }
    }

    //set peer view
    self.fPeersConnectField.enabled = YES;
    self.fPeersConnectLabel.enabled = YES;
    if (maxPeers != kInvalidValue)
    {
        self.fPeersConnectField.integerValue = maxPeers;
    }
    else
    {
        self.fPeersConnectField.stringValue = @"";
    }

    [self updateRelocationActionControls];
}

- (void)setUseSpeedLimit:(id)sender
{
    BOOL const upload = sender == self.fUploadLimitCheck;

    if (((NSButton*)sender).state == NSControlStateValueMixed)
    {
        [sender setState:NSControlStateValueOn];
    }
    BOOL const limit = ((NSButton*)sender).state == NSControlStateValueOn;

    for (Torrent* torrent in self.fTorrents)
    {
        [torrent setUseSpeedLimit:limit upload:upload];
    }

    NSTextField* field = upload ? self.fUploadLimitField : self.fDownloadLimitField;
    field.enabled = limit;
    if (limit)
    {
        [field selectText:self];
        [self.view.window makeKeyAndOrderFront:self];
    }

    NSTextField* label = upload ? self.fUploadLimitLabel : self.fDownloadLimitLabel;
    label.enabled = limit;

    [NSNotificationCenter.defaultCenter postNotificationName:@"UpdateOptionsNotification" object:self];
}

- (void)setUseGlobalSpeedLimit:(id)sender
{
    if (((NSButton*)sender).state == NSControlStateValueMixed)
    {
        [sender setState:NSControlStateValueOn];
    }
    BOOL const limit = ((NSButton*)sender).state == NSControlStateValueOn;

    for (Torrent* torrent in self.fTorrents)
    {
        torrent.usesGlobalSpeedLimit = limit;
    }

    [NSNotificationCenter.defaultCenter postNotificationName:@"UpdateOptionsNotification" object:self];
}

- (void)setSpeedLimit:(id)sender
{
    BOOL const upload = sender == self.fUploadLimitField;
    NSInteger const limit = [sender intValue];

    for (Torrent* torrent in self.fTorrents)
    {
        [torrent setSpeedLimit:limit upload:upload];
    }

    [NSNotificationCenter.defaultCenter postNotificationName:@"UpdateOptionsNotification" object:self];
}

- (void)setRatioSetting:(id)sender
{
    NSInteger setting;
    BOOL single = NO;
    switch ([sender indexOfSelectedItem])
    {
    case OptionPopupTypeLimit:
        setting = TR_RATIOLIMIT_SINGLE;
        single = YES;
        break;
    case OptionPopupTypeNoLimit:
        setting = TR_RATIOLIMIT_UNLIMITED;
        break;
    case OptionPopupTypeGlobal:
        setting = TR_RATIOLIMIT_GLOBAL;
        break;
    default:
        NSAssert1(NO, @"Unknown option selected in ratio popup: %ld", [sender indexOfSelectedItem]);
        return;
    }

    for (Torrent* torrent in self.fTorrents)
    {
        torrent.ratioSetting = static_cast<tr_ratiolimit>(setting);
    }

    self.fRatioLimitField.hidden = !single;
    if (single)
    {
        [self.fRatioLimitField selectText:self];
        [self.view.window makeKeyAndOrderFront:self];
    }

    self.fRatioLimitGlobalLabel.hidden = setting != TR_RATIOLIMIT_GLOBAL;

    [NSNotificationCenter.defaultCenter postNotificationName:@"UpdateOptionsNotification" object:self];
}

- (void)setRatioLimit:(id)sender
{
    CGFloat const limit = [sender floatValue];

    for (Torrent* torrent in self.fTorrents)
    {
        torrent.ratioLimit = limit;
    }

    [NSNotificationCenter.defaultCenter postNotificationName:@"UpdateOptionsNotification" object:self];
}

- (void)setIdleSetting:(id)sender
{
    NSInteger setting;
    BOOL single = NO;
    switch ([sender indexOfSelectedItem])
    {
    case OptionPopupTypeLimit:
        setting = TR_IDLELIMIT_SINGLE;
        single = YES;
        break;
    case OptionPopupTypeNoLimit:
        setting = TR_IDLELIMIT_UNLIMITED;
        break;
    case OptionPopupTypeGlobal:
        setting = TR_IDLELIMIT_GLOBAL;
        break;
    default:
        NSAssert1(NO, @"Unknown option selected in idle popup: %ld", [sender indexOfSelectedItem]);
        return;
    }

    for (Torrent* torrent in self.fTorrents)
    {
        torrent.idleSetting = static_cast<tr_idlelimit>(setting);
    }

    self.fIdleLimitField.hidden = !single;
    self.fIdleLimitLabel.hidden = !single;
    if (single)
    {
        [self.fIdleLimitField selectText:self];
        [self.view.window makeKeyAndOrderFront:self];
    }

    self.fIdleLimitGlobalLabel.hidden = setting != TR_IDLELIMIT_GLOBAL;

    [NSNotificationCenter.defaultCenter postNotificationName:@"UpdateOptionsNotification" object:self];
}

- (void)setIdleLimit:(id)sender
{
    NSUInteger const limit = [sender integerValue];

    for (Torrent* torrent in self.fTorrents)
    {
        torrent.idleLimitMinutes = limit;
    }

    [NSNotificationCenter.defaultCenter postNotificationName:@"UpdateOptionsNotification" object:self];
}

- (IBAction)setRemoveWhenSeedingCompletes:(id)sender
{
    if (((NSButton*)sender).state == NSControlStateValueMixed)
    {
        [sender setState:NSControlStateValueOn];
    }
    BOOL const enable = ((NSButton*)sender).state == NSControlStateValueOn;

    for (Torrent* torrent in self.fTorrents)
    {
        torrent.removeWhenFinishSeeding = enable;
    }

    [NSNotificationCenter.defaultCenter postNotificationName:@"UpdateOptionsNotification" object:self];
}

- (void)setPriority:(id)sender
{
    tr_priority_t priority;
    switch ([sender indexOfSelectedItem])
    {
    case OptionPopupPriorityHigh:
        priority = TR_PRI_HIGH;
        break;
    case OptionPopupPriorityNormal:
        priority = TR_PRI_NORMAL;
        break;
    case OptionPopupPriorityLow:
        priority = TR_PRI_LOW;
        break;
    default:
        NSAssert1(NO, @"Unknown option selected in priority popup: %ld", [sender indexOfSelectedItem]);
        return;
    }

    for (Torrent* torrent in self.fTorrents)
    {
        torrent.priority = priority;
    }

    [NSNotificationCenter.defaultCenter postNotificationName:@"UpdateUI" object:nil];

    [NSNotificationCenter.defaultCenter postNotificationName:@"UpdateOptionsNotification" object:self];
}

- (void)setPeersConnectLimit:(id)sender
{
    NSInteger limit = [sender intValue];

    for (Torrent* torrent in self.fTorrents)
    {
        torrent.maxPeerConnect = limit;
    }

    [NSNotificationCenter.defaultCenter postNotificationName:@"UpdateOptionsNotification" object:self];
}

- (BOOL)control:(NSControl*)control textShouldBeginEditing:(NSText*)fieldEditor
{
    self.fInitialString = control.stringValue;

    return YES;
}

- (BOOL)control:(NSControl*)control didFailToFormatString:(NSString*)string errorDescription:(NSString*)error
{
    NSBeep();
    if (self.fInitialString)
    {
        control.stringValue = self.fInitialString;
        self.fInitialString = nil;
    }
    return NO;
}

#pragma mark - Private

- (void)setupInfo
{
    if (self.fTorrents.count == 0)
    {
        self.fUploadLimitCheck.enabled = NO;
        self.fUploadLimitCheck.state = NSControlStateValueOff;
        self.fUploadLimitField.enabled = NO;
        self.fUploadLimitLabel.enabled = NO;
        self.fUploadLimitField.stringValue = @"";

        self.fDownloadLimitCheck.enabled = NO;
        self.fDownloadLimitCheck.state = NSControlStateValueOff;
        self.fDownloadLimitField.enabled = NO;
        self.fDownloadLimitLabel.enabled = NO;
        self.fDownloadLimitField.stringValue = @"";

        self.fGlobalLimitCheck.enabled = NO;
        self.fGlobalLimitCheck.state = NSControlStateValueOff;

        self.fPriorityPopUp.enabled = NO;
        [self.fPriorityPopUp selectItemAtIndex:-1];

        self.fRatioPopUp.enabled = NO;
        [self.fRatioPopUp selectItemAtIndex:-1];
        self.fRatioLimitField.hidden = YES;
        self.fRatioLimitField.stringValue = @"";
        self.fRatioLimitGlobalLabel.hidden = YES;

        self.fIdlePopUp.enabled = NO;
        [self.fIdlePopUp selectItemAtIndex:-1];
        self.fIdleLimitField.hidden = YES;
        self.fIdleLimitField.stringValue = @"";
        self.fIdleLimitLabel.hidden = YES;
        self.fIdleLimitGlobalLabel.hidden = YES;

        self.fRemoveSeedingCompleteCheck.enabled = NO;
        self.fRemoveSeedingCompleteCheck.state = NSControlStateValueOff;

        self.fPeersConnectField.enabled = NO;
        self.fPeersConnectField.stringValue = @"";
        self.fPeersConnectLabel.enabled = NO;
    }
    else
    {
        [self updateOptions];
    }

    [self updateRelocationActionControls];
}

- (void)setGlobalLabels
{
    NSString* global = [NSUserDefaults.standardUserDefaults boolForKey:@"RatioCheck"] ?
        [NSString stringForRatio:[NSUserDefaults.standardUserDefaults floatForKey:@"RatioLimit"]] :
        NSLocalizedString(@"disabled", "Info options -> global setting");
    self.fRatioLimitGlobalLabel.stringValue = global;

    //idle field
    NSString* globalIdle;
    if ([NSUserDefaults.standardUserDefaults boolForKey:@"IdleLimitCheck"])
    {
        NSInteger const globalMin = [NSUserDefaults.standardUserDefaults integerForKey:@"IdleLimitMinutes"];
        globalIdle = globalMin == 1 ?
            NSLocalizedString(@"1 minute", "Info options -> global setting") :
            [NSString localizedStringWithFormat:NSLocalizedString(@"%ld minutes", "Info options -> global setting"), globalMin];
    }
    else
    {
        globalIdle = NSLocalizedString(@"disabled", "Info options -> global setting");
    }
    self.fIdleLimitGlobalLabel.stringValue = globalIdle;
}

- (RelocationActionType)sharedRelocationActionType
{
    if (self.fTorrents.count == 0)
    {
        return RelocationActionTypeNone;
    }

    RelocationActionType actionType = RelocationActionTypeNone;
    for (Torrent* torrent in self.fTorrents)
    {
        RelocationActionType torrentAction = RelocationActionTypeNone;
        if (torrent.canRetryRelocation)
        {
            torrentAction = RelocationActionTypeRetry;
        }
        else if (torrent.canCancelRelocation)
        {
            torrentAction = RelocationActionTypeCancel;
        }
        else if (torrent.canResumeRelocation)
        {
            torrentAction = RelocationActionTypeResume;
        }

        if (torrentAction == RelocationActionTypeNone)
        {
            return RelocationActionTypeNone;
        }

        if (actionType == RelocationActionTypeNone)
        {
            actionType = torrentAction;
        }
        else if (actionType != torrentAction)
        {
            return RelocationActionTypeNone;
        }
    }

    return actionType;
}

- (void)updateRelocationActionControls
{
    BOOL const wasHidden = self.fRelocationActionButton.hidden;
    self.fRelocationActionType = [self sharedRelocationActionType];

    NSString* title = nil;
    switch (self.fRelocationActionType)
    {
    case RelocationActionTypeRetry:
        title = NSLocalizedString(@"Retry Relocation", "Info options -> relocation action");
        break;
    case RelocationActionTypeCancel:
        title = NSLocalizedString(@"Cancel Relocation", "Info options -> relocation action");
        break;
    case RelocationActionTypeResume:
        title = NSLocalizedString(@"Resume Relocation", "Info options -> relocation action");
        break;
    case RelocationActionTypeNone:
        break;
    }

    self.fRelocationActionButton.hidden = title == nil;
    if (title != nil)
    {
        self.fRelocationActionButton.title = title;
        self.fRelocationActionButton.enabled = YES;
    }

    if (wasHidden != self.fRelocationActionButton.hidden)
    {
        if (self.fSeedingViewHeightConstraint != nil)
        {
            CGFloat const extra = self.fRelocationActionButton.hidden ? 0.0 : self.fRelocationActionButton.fittingSize.height + kRelocationButtonSpacing;
            self.fSeedingViewHeightConstraint.constant = self.fSeedingViewBaseHeight + extra;
        }

        [self.view layoutSubtreeIfNeeded];
        [self checkWindowSize]; // the inspector window is sized from the two columns
    }
}

- (IBAction)handleRelocationAction:(id)sender
{
    for (Torrent* torrent in self.fTorrents)
    {
        switch (self.fRelocationActionType)
        {
        case RelocationActionTypeRetry:
            [torrent retryRelocation];
            break;
        case RelocationActionTypeCancel:
            [torrent cancelRelocation];
            break;
        case RelocationActionTypeResume:
            [torrent resumeRelocation];
            break;
        case RelocationActionTypeNone:
            break;
        }
    }

    [NSNotificationCenter.defaultCenter postNotificationName:@"UpdateOptions" object:self];
}

- (void)updateOptionsNotification:(NSNotification*)notification
{
    if (notification.object != self)
    {
        [self updateOptions];
    }

    [self updateRelocationActionControls];
}

@end
