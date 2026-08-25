// This file Copyright © TroyJ (Transmission fork).
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

#import "TorrentTableColumns.h"
#import "NSStringAdditions.h"
#import "Torrent.h"
#import "TorrentTableView.h"

NSString* const kTorrentTableViewModeDefaultsKey = @"TableView";
NSString* const kTorrentTableOutlineColumnIdentifier = @"Torrent";

namespace
{

struct ColumnSpec
{
    NSString* identifier;
    NSString* title;
    CGFloat width;
    NSString* sortType; // Controller's SortType value, or nil
    BOOL numeric;
};

// Order is the default column order; the user's reordering/widths persist through NSTableView autosave.
NSArray<NSDictionary*>* columnSpecs()
{
    static NSArray<NSDictionary*>* specs;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        specs = @[
            @{
                @"id" : @"Status",
                @"title" : NSLocalizedString(@"Status", "Table mode -> column"),
                @"width" : @110,
                @"sort" : @"State"
            },
            @{
                @"id" : @"Size",
                @"title" : NSLocalizedString(@"Size", "Table mode -> column"),
                @"width" : @80,
                @"sort" : @"Size",
                @"numeric" : @YES
            },
            @{
                @"id" : @"Done",
                @"title" : NSLocalizedString(@"Done", "Table mode -> column"),
                @"width" : @60,
                @"sort" : @"Progress",
                @"numeric" : @YES
            },
            @{
                @"id" : @"DownRate",
                @"title" : NSLocalizedString(@"Down", "Table mode -> column"),
                @"width" : @85,
                @"sort" : @"Activity",
                @"numeric" : @YES
            },
            @{
                @"id" : @"UpRate",
                @"title" : NSLocalizedString(@"Up", "Table mode -> column"),
                @"width" : @85,
                @"sort" : @"Activity",
                @"numeric" : @YES
            },
            @{
                @"id" : @"ETA",
                @"title" : NSLocalizedString(@"ETA", "Table mode -> column"),
                @"width" : @80,
                @"sort" : @"ETA",
                @"numeric" : @YES
            },
            @{
                @"id" : @"Ratio",
                @"title" : NSLocalizedString(@"Ratio", "Table mode -> column"),
                @"width" : @60,
                @"numeric" : @YES
            },
            @{
                @"id" : @"Peers",
                @"title" : NSLocalizedString(@"Peers", "Table mode -> column"),
                @"width" : @60,
                @"numeric" : @YES
            },
            @{
                @"id" : @"Added",
                @"title" : NSLocalizedString(@"Added", "Table mode -> column"),
                @"width" : @120,
                @"sort" : @"Date"
            },
            @{
                @"id" : @"Tracker",
                @"title" : NSLocalizedString(@"Tracker", "Table mode -> column"),
                @"width" : @140,
                @"sort" : @"Tracker"
            },
        ];
    });
    return specs;
}

NSDictionary* specForIdentifier(NSString* identifier)
{
    for (NSDictionary* spec in columnSpecs())
    {
        if ([spec[@"id"] isEqualToString:identifier])
        {
            return spec;
        }
    }
    return nil;
}

} // namespace

@implementation TorrentTableColumns

+ (BOOL)isEnabled
{
    return [NSUserDefaults.standardUserDefaults boolForKey:kTorrentTableViewModeDefaultsKey];
}

+ (void)applyToTableView:(TorrentTableView*)tableView enabled:(BOOL)enabled
{
    // remove ours (never the xib's outline column)
    for (NSTableColumn* column in [tableView.tableColumns copy])
    {
        if (![column.identifier isEqualToString:kTorrentTableOutlineColumnIdentifier])
        {
            [tableView removeTableColumn:column];
        }
    }

    NSTableColumn* outlineColumn = [tableView tableColumnWithIdentifier:kTorrentTableOutlineColumnIdentifier];

    if (!enabled)
    {
        tableView.headerView = nil;
        tableView.autosaveTableColumns = NO;
        tableView.autosaveName = nil;
        tableView.allowsColumnResizing = NO;
        tableView.allowsColumnReordering = NO;
        tableView.sortDescriptors = @[];
        outlineColumn.title = @"";
        outlineColumn.sortDescriptorPrototype = nil;
        [tableView sizeLastColumnToFit];
        return;
    }

    outlineColumn.title = NSLocalizedString(@"Name", "Table mode -> column");
    outlineColumn.sortDescriptorPrototype = [NSSortDescriptor sortDescriptorWithKey:@"Name" ascending:YES];
    outlineColumn.minWidth = 160;

    for (NSDictionary* spec in columnSpecs())
    {
        NSTableColumn* column = [[NSTableColumn alloc] initWithIdentifier:spec[@"id"]];
        column.title = spec[@"title"];
        column.width = [spec[@"width"] doubleValue];
        column.minWidth = 40;
        column.maxWidth = 600;
        column.resizingMask = NSTableColumnUserResizingMask;
        column.editable = NO;
        if (NSString* sortType = spec[@"sort"])
        {
            column.sortDescriptorPrototype = [NSSortDescriptor sortDescriptorWithKey:sortType ascending:YES];
        }
        [tableView addTableColumn:column];
    }

    tableView.headerView = [[NSTableHeaderView alloc] init];
    tableView.allowsColumnResizing = YES;
    tableView.allowsColumnReordering = YES;
    tableView.autosaveName = @"TorrentTableModeColumns";
    tableView.autosaveTableColumns = YES;
}

+ (NSString*)stringForColumnIdentifier:(NSString*)identifier torrent:(Torrent*)torrent
{
    if (torrent == nil)
    {
        return nil;
    }
    if ([identifier isEqualToString:@"Status"])
    {
        return torrent.shortStatusString;
    }
    if ([identifier isEqualToString:@"Size"])
    {
        return [NSString stringForFileSize:torrent.size];
    }
    if ([identifier isEqualToString:@"Done"])
    {
        return [NSString localizedStringWithFormat:@"%.1f%%", torrent.progress * 100.0];
    }
    if ([identifier isEqualToString:@"DownRate"])
    {
        return torrent.downloadRate > 0.0 ? [NSString stringForSpeed:torrent.downloadRate] : @"";
    }
    if ([identifier isEqualToString:@"UpRate"])
    {
        return torrent.uploadRate > 0.0 ? [NSString stringForSpeed:torrent.uploadRate] : @"";
    }
    if ([identifier isEqualToString:@"ETA"])
    {
        return torrent.allDownloaded || !torrent.active ? @"" : torrent.remainingTimeString;
    }
    if ([identifier isEqualToString:@"Ratio"])
    {
        return [NSString stringForRatio:torrent.ratio];
    }
    if ([identifier isEqualToString:@"Peers"])
    {
        return torrent.active ? [NSString stringWithFormat:@"%lu", (unsigned long)torrent.totalPeersConnected] : @"";
    }
    if ([identifier isEqualToString:@"Added"])
    {
        static NSDateFormatter* formatter;
        static dispatch_once_t once;
        dispatch_once(&once, ^{
            formatter = [[NSDateFormatter alloc] init];
            formatter.dateStyle = NSDateFormatterShortStyle;
            formatter.timeStyle = NSDateFormatterShortStyle;
        });
        NSDate* added = torrent.dateAdded;
        return added != nil ? [formatter stringFromDate:added] : @"";
    }
    if ([identifier isEqualToString:@"Tracker"])
    {
        return torrent.trackerSortKey ?: @"";
    }
    return nil;
}

+ (NSTextAlignment)alignmentForColumnIdentifier:(NSString*)identifier
{
    NSDictionary* spec = specForIdentifier(identifier);
    return [spec[@"numeric"] boolValue] ? NSTextAlignmentRight : NSTextAlignmentLeft;
}

+ (NSString*)sortTypeForColumnIdentifier:(NSString*)identifier
{
    if ([identifier isEqualToString:kTorrentTableOutlineColumnIdentifier])
    {
        return @"Name";
    }
    return specForIdentifier(identifier)[@"sort"];
}

+ (NSString*)columnIdentifierForSortType:(NSString*)sortType
{
    if ([sortType isEqualToString:@"Name"])
    {
        return kTorrentTableOutlineColumnIdentifier;
    }
    for (NSDictionary* spec in columnSpecs())
    {
        if ([spec[@"sort"] isEqualToString:sortType])
        {
            return spec[@"id"];
        }
    }
    return nil;
}

@end
