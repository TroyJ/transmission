// This file Copyright © TroyJ (Transmission fork).
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

#import <AppKit/AppKit.h>

@class Torrent;
@class TorrentTableView;

/// qB-style table mode (fork feature, additive): extra NSTableColumns added
/// next to the existing "Torrent" outline column, with a visible header and
/// header-click sorting. The classic single-column rows stay untouched; this
/// only installs/removes columns and formats their text. See
/// plans/qb-style-table-view.md.
@interface TorrentTableColumns : NSObject

extern NSString* const kTorrentTableViewModeDefaultsKey; ///< "TableView" (BOOL)
extern NSString* const kTorrentTableOutlineColumnIdentifier; ///< the xib's "Torrent" column

+ (BOOL)isEnabled;

/// Add or remove the table-mode columns on `tableView` to match `enabled`.
+ (void)applyToTableView:(TorrentTableView*)tableView enabled:(BOOL)enabled;

/// Text for a table-mode column, or nil for the outline column / group rows.
+ (NSString*)stringForColumnIdentifier:(NSString*)identifier torrent:(Torrent*)torrent;

/// Right-align numeric columns.
+ (NSTextAlignment)alignmentForColumnIdentifier:(NSString*)identifier;

/// The Controller "Sort" defaults value a header click on `identifier` maps to, or nil if that column does not sort.
+ (NSString*)sortTypeForColumnIdentifier:(NSString*)identifier;

/// Reverse lookup, to keep the header's sort indicator in step with the View > Sort menu.
+ (NSString*)columnIdentifierForSortType:(NSString*)sortType;

@end
