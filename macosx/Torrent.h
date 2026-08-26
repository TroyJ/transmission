// This file Copyright © Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

#import <Foundation/Foundation.h>
#import <Quartz/Quartz.h>

#include <libtransmission/transmission.h>

@class FileListNode;

@interface TorrentMainWindowSnapshot : NSObject

@property(nonatomic, readonly, copy) NSString* hashString;
@property(nonatomic, readonly) int torrentId; ///< so a re-added torrent with the same hash never receives a stale snapshot
@property(nonatomic, readonly) tr_stat stat;
@property(nonatomic, readonly, copy) NSString* name;
@property(nonatomic, getter=isMagnet, readonly) BOOL magnet;
@property(nonatomic, getter=isFolder, readonly) BOOL folder;
@property(nonatomic, readonly) uint64_t size;
@property(nonatomic, readonly) NSInteger pieceSize;
@property(nonatomic, readonly) NSInteger pieceCount;
@property(nonatomic, readonly) BOOL privateTorrent;
@property(nonatomic, readonly) tr_priority_t priority;
@property(nonatomic, readonly, copy) NSArray<NSString*>* allTrackersFlat;
@property(nonatomic, readonly) NSString* trackerSortKey;
@property(nonatomic, readonly) BOOL canManualAnnounce;
@property(nonatomic, readonly) BOOL canRetryRelocation;
@property(nonatomic, readonly) BOOL canResumeRelocation;
@property(nonatomic, readonly) BOOL canCancelRelocation;
@property(nonatomic, readonly) NSData* piecePercentData;
@property(nonatomic, readonly) BOOL includesPiecePercentData;

/// Phase 2: what the inspector needs, sampled under the lock for the selected
/// torrents while the inspector is visible. All nil/NO when not included.
@property(nonatomic, readonly) BOOL includesInspectorData;
@property(nonatomic, readonly, copy) NSArray<NSDictionary*>* peers;
@property(nonatomic, readonly, copy) NSArray<NSDictionary*>* webSeeds;
@property(nonatomic, readonly, copy) NSData* trackerViews; ///< tr_tracker_view[]
@property(nonatomic, readonly, copy) NSData* fileHave; ///< uint64_t[fileCount]
@property(nonatomic, readonly, copy) NSData* availability; ///< int8_t[min(pieceCount, 18*18)]
@property(nonatomic, readonly, copy) NSData* amountFinishedCells; ///< float[min(pieceCount, 18*18)]
@property(nonatomic, readonly, copy) NSString* dataLocation; ///< nil when no data on disk
@property(nonatomic, readonly) NSUInteger fileCount;

- (instancetype)init NS_UNAVAILABLE;

@end

typedef NS_ENUM(NSUInteger, TorrentDeterminationType) { TorrentDeterminationAutomatic = 0, TorrentDeterminationUserSpecified };

extern NSString* const kTorrentDidChangeGroupNotification;

/// Phase 1b: a conservative marker that a mutation has been handed to the
/// controller's command queue and has not yet been reflected by a sample.
/// Main-thread only. It disables conflicting actions and labels the row; it is
/// not shadow state -- the backend stays authoritative.
typedef NS_ENUM(NSInteger, TorrentPendingCommand) {
    TorrentPendingCommandNone = 0,
    TorrentPendingCommandStart,
    TorrentPendingCommandStop,
    TorrentPendingCommandVerify,
    TorrentPendingCommandAnnounce,
    TorrentPendingCommandRelocate,
    TorrentPendingCommandRemove,
};

@interface Torrent : NSObject<NSCopying, QLPreviewItem>

@property(nonatomic) TorrentPendingCommand pendingCommand;
@property(nonatomic, readonly) BOOL hasPendingCommand;
/// libtransmission id, for looking the live object up again on another queue
/// (tr_torrentFindFromId under the session lock). -1 once detached.
@property(nonatomic, readonly) int torrentId;
/// Forget the live handle (main thread) before an asynchronous remove.
- (void)detachTorrentStruct;

/// Off-main half of the remaining-disk-space check: statfs on the download
/// volume, which can stall. YES means "fine to start"; NO means the main
/// thread should ask via -presentRemainingDiskSpaceAlert.
// Two halves of the free-space check (phase 1b, item 3 of the blocking
// taxonomy): the first reads the torrent on the session thread and returns
// nil when no check is needed; the second does the statfs on the caller's
// own queue. Neither takes the session lock.
- (NSDictionary*)remainingDiskSpaceNeedForStruct:(tr_torrent*)torrentStruct;
+ (BOOL)hasEnoughRemainingDiskSpaceForNeed:(NSDictionary*)need;
/// Main-thread half: shows the alert; YES means "download anyway".
- (BOOL)presentRemainingDiskSpaceAlert;
/// Both halves, synchronously (kept for the magnet-metadata path).
- (BOOL)alertForRemainingDiskSpace;

- (instancetype)initWithPath:(NSString*)path
                    location:(NSString*)location
           deleteTorrentFile:(BOOL)torrentDelete
                         lib:(tr_session*)lib;
- (instancetype)initWithTorrentStruct:(tr_torrent*)torrentStruct location:(NSString*)location lib:(tr_session*)lib;
/// Phase 1c: with a snapshot already built under the session lock, the init does no stat pull of its own.
- (instancetype)initWithTorrentStruct:(tr_torrent*)torrentStruct
                             location:(NSString*)location
                                  lib:(tr_session*)lib
                             snapshot:(TorrentMainWindowSnapshot*)snapshot;
// Runs on the session thread; returns the disk half (a stat and an xattr on
// the data volume) for the caller to run on its own queue, or nil.
+ (dispatch_block_t)timeMachineExcludeUpdateForStruct:(tr_torrent*)torrentStruct;
/// Where the data is, resolved from the live struct (session thread / command queue); nil when nothing is on disk.
+ (NSString*)dataLocationForTorrentStruct:(tr_torrent*)torrentStruct;
- (instancetype)initWithMagnetAddress:(NSString*)address location:(NSString*)location lib:(tr_session*)lib;
- (void)setResumeStatusForTorrent:(Torrent*)torrent withHistory:(NSDictionary*)history forcePause:(BOOL)pause;

@property(nonatomic, readonly) NSDictionary* history;

- (void)closeRemoveTorrent:(BOOL)trashFiles;
- (BOOL)canMoveTorrentDataFileTo:(NSString*)folder;
// Session-thread half of a move; returns the disk half (clearing the Time
// Machine flag on the source) for the caller's queue, or nil.
- (dispatch_block_t)moveTorrentStruct:(tr_torrent*)torrentStruct dataFileTo:(NSString*)folder;
// Session-thread half of a removal; returns the disk half (clearing the Time
// Machine flag if the data is kept) for the caller's queue, or nil.
+ (dispatch_block_t)removeTorrentStruct:(tr_torrent*)torrentStruct trashFiles:(BOOL)trashFiles;

- (void)changeDownloadFolderBeforeUsing:(NSString*)folder determinationType:(TorrentDeterminationType)determinationType;

@property(nonatomic, readonly) NSString* currentDirectory;

- (void)getAvailability:(int8_t*)tab size:(int)size;
- (void)getAmountFinished:(float*)tab size:(int)size;
@property(nonatomic) NSIndexSet* previousFinishedPieces;
@property(nonatomic, readonly) NSData* mainWindowPiecePercentData;

- (void)update;
+ (TorrentMainWindowSnapshot*)mainWindowSnapshotForTorrentStruct:(tr_torrent*)torrentStruct includePieces:(BOOL)includePieces;
+ (TorrentMainWindowSnapshot*)mainWindowSnapshotForTorrentStruct:(tr_torrent*)torrentStruct
                                                   includePieces:(BOOL)includePieces
                                                includeInspector:(BOOL)includeInspector;
/// Phase 2: YES while the inspector getters (peers, trackers, file progress,
/// availability, data location) can answer from a sampled cache.
@property(nonatomic, readonly) BOOL hasInspectorSnapshot;
- (void)dropInspectorSnapshot;
- (TorrentMainWindowSnapshot*)createMainWindowSnapshotIncludingPieces:(BOOL)includePieces;
- (void)applyMainWindowSnapshot:(TorrentMainWindowSnapshot*)snapshot;

- (void)startTransferIgnoringQueue:(BOOL)ignoreQueue;
- (void)startTransferNoQueue;
- (void)startTransfer;
- (void)startMagnetTransferAfterMetaDownload;
- (void)stopTransfer;
- (void)sleep;
- (void)wakeUp;
- (void)idleLimitHitWithSnapshot:(TorrentMainWindowSnapshot*)snapshot dataLocation:(NSString*)dataLocation;
- (void)ratioLimitHitWithSnapshot:(TorrentMainWindowSnapshot*)snapshot dataLocation:(NSString*)dataLocation;
- (void)metadataRetrieved;
- (void)completenessChange:(tr_completeness)status
                wasRunning:(BOOL)wasRunning
                  snapshot:(TorrentMainWindowSnapshot*)snapshot
              dataLocation:(NSString*)dataLocation;

@property(nonatomic) NSUInteger queuePosition;

- (void)manualAnnounce;
@property(nonatomic, readonly) BOOL canManualAnnounce;

- (void)resetCache;

@property(nonatomic, getter=isMagnet, readonly) BOOL magnet;
@property(nonatomic, readonly) NSString* magnetLink;

@property(nonatomic, readonly) CGFloat ratio;
@property(nonatomic) tr_ratiolimit ratioSetting;
@property(nonatomic) CGFloat ratioLimit;
@property(nonatomic, readonly) CGFloat progressStopRatio;

@property(nonatomic) tr_idlelimit idleSetting;
@property(nonatomic) NSUInteger idleLimitMinutes;

- (BOOL)usesSpeedLimit:(BOOL)upload;
- (void)setUseSpeedLimit:(BOOL)use upload:(BOOL)upload;
- (NSUInteger)speedLimit:(BOOL)upload;
- (void)setSpeedLimit:(NSUInteger)limit upload:(BOOL)upload;
@property(nonatomic) BOOL usesGlobalSpeedLimit;

@property(nonatomic) uint16_t maxPeerConnect;

@property(nonatomic) BOOL removeWhenFinishSeeding;

@property(nonatomic, readonly) BOOL waitingToStart;

@property(nonatomic) tr_priority_t priority;

+ (BOOL)trashFile:(NSString*)path error:(NSError**)error;
- (void)moveTorrentDataFileTo:(NSString*)folder;
- (void)copyTorrentFileTo:(NSString*)path;
- (void)retryRelocation;
- (void)resumeRelocation;
- (void)cancelRelocation;

@property(nonatomic, readonly) BOOL canRetryRelocation;
@property(nonatomic, readonly) BOOL canResumeRelocation;
@property(nonatomic, readonly) BOOL canCancelRelocation;
@property(nonatomic, readonly) tr_torrent_relocation_state relocationState;

@property(nonatomic, readonly) BOOL alertForRemainingDiskSpace;

@property(nonatomic, readonly) NSImage* icon;

@property(nonatomic, readonly) NSString* name;
@property(nonatomic, getter=isFolder, readonly) BOOL folder;
@property(nonatomic, readonly) uint64_t size;
@property(nonatomic, readonly) uint64_t sizeLeft;

@property(nonatomic, readonly) NSMutableArray* allTrackerStats;
@property(nonatomic, readonly) NSArray<NSString*>* allTrackersFlat; //used by GroupRules
- (BOOL)addTrackerToNewTier:(NSString*)tracker;
- (void)removeTrackers:(NSSet*)trackers;

@property(nonatomic, readonly) NSString* comment;
@property(nonatomic, readonly) NSString* creator;
@property(nonatomic, readonly) NSDate* dateCreated;

@property(nonatomic, readonly) NSInteger pieceSize;
@property(nonatomic, readonly) NSInteger pieceCount;
@property(nonatomic, readonly) NSString* hashString;
@property(nonatomic, readonly) BOOL privateTorrent;

@property(nonatomic, readonly) NSString* torrentLocation;
@property(nonatomic, readonly) NSString* dataLocation;
@property(nonatomic, readonly) NSString* lastKnownDataLocation;
- (NSString*)fileLocation:(FileListNode*)node;

- (void)renameTorrent:(NSString*)newName completionHandler:(void (^)(BOOL didRename))completionHandler;
- (void)renameFileNode:(FileListNode*)node
              withName:(NSString*)newName
     completionHandler:(void (^)(BOOL didRename))completionHandler;

@property(nonatomic, readonly) time_t eta;
@property(nonatomic, readonly) CGFloat progress;
@property(nonatomic, readonly) CGFloat progressDone;
@property(nonatomic, readonly) CGFloat progressLeft;
@property(nonatomic, readonly) CGFloat checkingProgress;

@property(nonatomic, readonly) CGFloat availableDesired;

/// True if non-paused. Running.
@property(nonatomic, getter=isActive, readonly) BOOL active;
/// True if downloading or uploading.
@property(nonatomic, getter=isTransmitting, readonly) BOOL transmitting;
@property(nonatomic, getter=isSeeding, readonly) BOOL seeding;
@property(nonatomic, getter=isChecking, readonly) BOOL checking;
@property(nonatomic, getter=isCheckingWaiting, readonly) BOOL checkingWaiting;
@property(nonatomic, readonly) BOOL allDownloaded;
@property(nonatomic, getter=isComplete, readonly) BOOL complete;
@property(nonatomic, getter=isFinishedSeeding, readonly) BOOL finishedSeeding;
@property(nonatomic, getter=isError, readonly) BOOL error;
@property(nonatomic, getter=isAnyErrorOrWarning, readonly) BOOL anyErrorOrWarning;
@property(nonatomic, readonly) NSString* errorMessage;

@property(nonatomic, readonly) NSArray<NSDictionary*>* peers;

@property(nonatomic, readonly) NSUInteger webSeedCount;
@property(nonatomic, readonly) NSArray<NSDictionary*>* webSeeds;

@property(nonatomic, readonly) NSString* progressString;
@property(nonatomic, readonly) NSString* statusString;
@property(nonatomic, readonly) NSString* shortStatusString;
@property(nonatomic, readonly) NSString* remainingTimeString;

@property(nonatomic, readonly) NSString* stateString;
@property(nonatomic, readonly) NSUInteger totalPeersConnected;
@property(nonatomic, readonly) NSUInteger totalPeersTracker;
@property(nonatomic, readonly) NSUInteger totalPeersIncoming;
@property(nonatomic, readonly) NSUInteger totalPeersCache;
@property(nonatomic, readonly) NSUInteger totalPeersPex;
@property(nonatomic, readonly) NSUInteger totalPeersDHT;
@property(nonatomic, readonly) NSUInteger totalPeersLocal;
@property(nonatomic, readonly) NSUInteger totalPeersLTEP;

@property(nonatomic, readonly) NSUInteger totalKnownPeersTracker;
@property(nonatomic, readonly) NSUInteger totalKnownPeersIncoming;
@property(nonatomic, readonly) NSUInteger totalKnownPeersCache;
@property(nonatomic, readonly) NSUInteger totalKnownPeersPex;
@property(nonatomic, readonly) NSUInteger totalKnownPeersDHT;
@property(nonatomic, readonly) NSUInteger totalKnownPeersLocal;
@property(nonatomic, readonly) NSUInteger totalKnownPeersLTEP;

@property(nonatomic, readonly) NSUInteger peersSendingToUs;
@property(nonatomic, readonly) NSUInteger peersGettingFromUs;

@property(nonatomic, readonly) CGFloat downloadRate;
@property(nonatomic, readonly) CGFloat uploadRate;
@property(nonatomic, readonly) CGFloat totalRate;
@property(nonatomic, readonly) uint64_t haveVerified;
@property(nonatomic, readonly) uint64_t haveTotal;
@property(nonatomic, readonly) uint64_t totalSizeSelected;
@property(nonatomic, readonly) uint64_t downloadedTotal;
@property(nonatomic, readonly) uint64_t uploadedTotal;
@property(nonatomic, readonly) uint64_t failedHash;

@property(nonatomic, readonly) NSInteger groupValue;
- (void)setGroupValue:(NSInteger)groupValue determinationType:(TorrentDeterminationType)determinationType;
;
@property(nonatomic, readonly) NSInteger groupOrderValue;
- (void)checkGroupValueForRemoval:(NSNotification*)notification;

@property(nonatomic, readonly) NSArray<FileListNode*>* fileList;
@property(nonatomic, readonly) NSArray<FileListNode*>* flatFileList;
@property(nonatomic, readonly) NSUInteger fileCount;

//methods require fileStats to have been updated recently to be accurate
- (CGFloat)fileProgress:(FileListNode*)node;
- (BOOL)canChangeDownloadCheckForFiles:(NSIndexSet*)indexSet;
- (NSControlStateValue)checkForFiles:(NSIndexSet*)indexSet;
- (void)setFileCheckState:(NSControlStateValue)state forIndexes:(NSIndexSet*)indexSet;
- (void)setFilePriority:(tr_priority_t)priority forIndexes:(NSIndexSet*)indexSet;
- (BOOL)hasFilePriority:(tr_priority_t)priority forIndexes:(NSIndexSet*)indexSet;
- (NSSet*)filePrioritiesForIndexes:(NSIndexSet*)indexSet;

@property(nonatomic, readonly) NSDate* dateAdded;
@property(nonatomic, readonly) NSDate* dateCompleted;
@property(nonatomic, readonly) NSDate* dateActivity;
@property(nonatomic, readonly) NSDate* dateActivityOrAdd;

@property(nonatomic, readonly) NSInteger secondsDownloading;
@property(nonatomic, readonly) NSInteger secondsSeeding;

@property(nonatomic, readonly) NSInteger stalledMinutes;
/// True if the torrent is running, but has been idle for long enough to be considered stalled.
@property(nonatomic, getter=isStalled, readonly) BOOL stalled;

- (void)updateTimeMachineExclude;

@property(nonatomic, readonly) NSInteger stateSortKey;
@property(nonatomic, readonly) NSString* trackerSortKey;

@property(nonatomic, readonly) tr_torrent* torrentStruct;

@end
