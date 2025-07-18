// SPDX-License-Identifier: Apache-2.0
// Copyright Authors of Cilium

package allocator

import (
	"context"
	"errors"
	"fmt"
	"log/slog"

	"github.com/cilium/cilium/pkg/backoff"
	"github.com/cilium/cilium/pkg/idpool"
	"github.com/cilium/cilium/pkg/kvstore"
	"github.com/cilium/cilium/pkg/lock"
	"github.com/cilium/cilium/pkg/logging"
	"github.com/cilium/cilium/pkg/logging/logfields"
	"github.com/cilium/cilium/pkg/option"
	"github.com/cilium/cilium/pkg/rate"
	"github.com/cilium/cilium/pkg/time"
)

var (
	subsysLogAttr = []any{logfields.LogSubsys, "allocator"}
)

const (
	// DefaultSyncInterval is the default value for the periodic synchronization
	// of the allocated identities.
	DefaultSyncInterval = 5 * time.Minute

	// defaultMaxAllocAttempts is the default number of attempted allocation
	// requests performed before failing.
	defaultMaxAllocAttempts = 16
)

// Allocator is a distributed ID allocator backed by a KVstore. It maps
// arbitrary keys to identifiers. Multiple users on different cluster nodes can
// in parallel request the ID for keys and are guaranteed to retrieve the same
// ID for an identical key.
//
// While the details of how keys are stored is delegated to Backend
// implementations, some expectations exist. See pkg/kvstore/allocator for
// details about the kvstore implementation.
//
// A node takes a reference to an identity when it is in-use on that node, and
// the identity remains in-use if there is any node reference to it. When an
// identity no longer has any node references, it may be garbage collected. No
// guarantees are made at that point and the numeric identity may be reused.
// Note that the numeric IDs are selected locally and verified with the Backend.
//
// Lookup ID by key:
//  1. Return ID from local cache updated by watcher (no Backend interactions)
//  2. Do ListPrefix() on slave key, return the first result that matches the exact
//     prefix.
//
// Lookup key by ID:
//  1. Return key from local cache updated by watcher (no Backend interactions)
//  2. Do Get() on master key, return result
//
// Allocate:
//  1. Check local key cache, increment, and return if key is already in use
//     locally (no Backend interactions)
//  2. Check local cache updated by watcher, if...
//
// ... match found:
//
//	2.1 Create a new slave key. This operation is potentially racy as the master
//	    key can be removed in the meantime.
//	    - etcd: Create is made conditional on existence of master key
//
// ... match not found:
//
//	2.1 Select new unused id from local cache
//	2.2 Create a new master key with the condition that it may not exist
//	2.3 Create a new slave key
//
// 1.1. If found, increment and return (no Backend interactions)
// 2. Lookup ID by key in local cache or via first slave key found in Backend
//
// Release:
//  1. Reduce local reference count until last use (no Backend interactions)
//  2. Delete slave key (basePath/value/key1/node1)
//     This automatically guarantees that when the last node has released the
//     key, the key is no longer found by Get()
//  3. If the node goes down, all slave keys of that node are removed after
//     the TTL expires (auto release).
type Allocator struct {
	logger *slog.Logger

	// events is a channel which will receive AllocatorEvent as IDs are
	// added, modified or removed from the allocator
	events AllocatorEventSendChan

	// keyType is an instance of the type to be used as allocator key.
	keyType AllocatorKey

	// min is the lower limit when allocating IDs. The allocator will never
	// allocate an ID lesser than this value.
	min idpool.ID

	// max is the upper limit when allocating IDs. The allocator will never
	// allocate an ID greater than this value.
	max idpool.ID

	// prefixMask if set, will be ORed to all selected IDs prior to
	// allocation
	prefixMask idpool.ID

	// localKeys contains all keys including their reference count for keys
	// which have been allocated and are in local use
	localKeys *localKeys

	// backoffTemplate is the backoff configuration while allocating
	backoffTemplate backoff.Exponential

	// slaveKeysMutex protects the concurrent access of the slave key by this
	// agent.
	slaveKeysMutex lock.Mutex

	// mainCache is the main cache, representing the allocator contents of
	// the primary kvstore connection
	mainCache cache

	// remoteCachesMutex protects accesse to remoteCaches
	remoteCachesMutex lock.RWMutex

	// remoteCaches is the list of additional remote caches being watched
	// in addition to the main cache
	remoteCaches map[string]*remoteCache

	// stopGC is the channel used to stop the garbage collector
	stopGC chan struct{}

	// initialListDone is a channel that is closed when the initial
	// synchronization has completed
	initialListDone waitChan

	// idPool maintains a pool of available ids for allocation.
	idPool *idpool.IDPool

	// enableMasterKeyProtection if true, causes master keys that are still in
	// local use to be automatically re-created
	enableMasterKeyProtection bool

	// disableGC disables the garbage collector
	disableGC bool

	// disableAutostart prevents starting the allocator when it is initialized
	disableAutostart bool

	// operatorIDManagement indicates if cilium-operator is managing Cilium Identities.
	operatorIDManagement bool

	// maxAllocAttempts is the number of attempted allocation requests
	// performed before failing.
	maxAllocAttempts int

	// syncInterval is the interval for local keys refresh.
	syncInterval time.Duration

	// cacheValidators implement extra validations of retrieved identities, e.g.,
	// to ensure that they belong to the expected range.
	cacheValidators []CacheValidator

	// backend is the upstream, shared, backend to which we syncronize local
	// information
	backend Backend
}

// AllocatorOption is the base type for allocator options
type AllocatorOption func(*Allocator)

// CacheValidator is the type of the validation functions triggered to filter out
// invalid notification events.
type CacheValidator func(kind AllocatorChangeKind, id idpool.ID, key AllocatorKey) error

// NewAllocatorForGC returns an allocator that can be used to run RunGC()
//
// The allocator can be configured by passing in additional options:
//   - WithMin(id) - minimum ID to allocate (default: 1)
//   - WithMax(id) - maximum ID to allocate (default max(uint64))
func NewAllocatorForGC(rootLogger *slog.Logger, backend Backend, opts ...AllocatorOption) *Allocator {
	a := &Allocator{
		logger:  rootLogger.With(subsysLogAttr...),
		backend: backend,
		min:     idpool.ID(1),
		max:     idpool.ID(^uint64(0)),
	}

	for _, fn := range opts {
		fn(a)
	}

	return a
}

type GCStats struct {
	// Alive is the number of identities alive
	Alive int

	// Deleted is the number of identities deleted
	Deleted int
}

// Backend represents clients to remote ID allocation systems, such as KV
// Stores. These are used to coordinate key->ID allocation between cilium
// nodes.
type Backend interface {
	// DeleteAllKeys will delete all keys. It is used in tests.
	DeleteAllKeys(ctx context.Context)

	// DeleteID deletes the identity with the given ID
	DeleteID(ctx context.Context, id idpool.ID) error

	// AllocateID creates a new key->ID association. This is expected to be a
	// create-only operation, and the ID may be allocated by another node. An
	// error in that case is not expected to be fatal. The actual ID is obtained
	// by Allocator from the local idPool, which is updated with used-IDs as the
	// Backend makes calls to the handler in ListAndWatch.
	// The implementation of the backend might return an AllocatorKey that is
	// a copy of 'key' with an internal reference of the backend key or, if it
	// doesn't use the internal reference of the backend key it simply returns
	// 'key'. In case of an error the returned 'AllocatorKey' should be nil.
	AllocateID(ctx context.Context, id idpool.ID, key AllocatorKey) (AllocatorKey, error)

	// AllocateIDIfLocked behaves like AllocateID but when lock is non-nil the
	// operation proceeds only if it is still valid.
	// The implementation of the backend might return an AllocatorKey that is
	// a copy of 'key' with an internal reference of the backend key or, if it
	// doesn't use the internal reference of the backend key it simply returns
	// 'key'. In case of an error the returned 'AllocatorKey' should be nil.
	AllocateIDIfLocked(ctx context.Context, id idpool.ID, key AllocatorKey, lock kvstore.KVLocker) (AllocatorKey, error)

	// AcquireReference records that this node is using this key->ID mapping.
	// This is distinct from any reference counting within this agent; only one
	// reference exists for this node for any number of managed endpoints using
	// it.
	// The semantics of cleaning up stale references is delegated to the Backend
	// implementation. RunGC may need to be invoked.
	// This can race, and so lock can be provided (via a Lock call, below).
	AcquireReference(ctx context.Context, id idpool.ID, key AllocatorKey, lock kvstore.KVLocker) error

	// Release releases the use of an ID associated with the provided key. It
	// does not guard against concurrent calls to
	// releases.Release(ctx context.Context, key AllocatorKey) (err error)
	Release(ctx context.Context, id idpool.ID, key AllocatorKey) (err error)

	// UpdateKey refreshes the record that this node is using this key -> id
	// mapping. When reliablyMissing is set it will also recreate missing master or
	// slave keys.
	UpdateKey(ctx context.Context, id idpool.ID, key AllocatorKey, reliablyMissing bool) error

	// UpdateKeyIfLocked behaves like UpdateKey but when lock is non-nil the operation proceeds only if it is still valid.
	UpdateKeyIfLocked(ctx context.Context, id idpool.ID, key AllocatorKey, reliablyMissing bool, lock kvstore.KVLocker) error

	// Get returns the allocated ID for this key as seen by the Backend. This may
	// have been created by other agents.
	Get(ctx context.Context, key AllocatorKey) (idpool.ID, error)

	// GetIfLocked behaves like Get, but but when lock is non-nil the
	// operation proceeds only if it is still valid.
	GetIfLocked(ctx context.Context, key AllocatorKey, lock kvstore.KVLocker) (idpool.ID, error)

	// GetByID returns the key associated with this ID, as seen by the Backend.
	// This may have been created by other agents.
	GetByID(ctx context.Context, id idpool.ID) (AllocatorKey, error)

	// Lock provides an opaque lock object that can be used, later, to ensure
	// that the key has not changed since the lock was created. This can be done
	// with GetIfLocked.
	Lock(ctx context.Context, key AllocatorKey) (kvstore.KVLocker, error)

	// ListIDs returns the IDs of all identities currently stored in the backend
	ListIDs(ctx context.Context) (identityIDs []idpool.ID, err error)

	// ListAndWatch begins synchronizing the local Backend instance with its
	// remote.
	ListAndWatch(ctx context.Context, handler CacheMutations)

	// RunGC reaps stale or unused identities within the Backend and makes them
	// available for reuse. It is used by the cilium-operator and is not invoked
	// by cilium-agent.
	// Note: not all Backend implemenations rely on this, such as the kvstore
	// backends, and may use leases to expire keys.
	RunGC(ctx context.Context, rateLimit *rate.Limiter, staleKeysPrevRound map[string]uint64, minID idpool.ID, maxID idpool.ID) (map[string]uint64, *GCStats, error)

	// RunLocksGC reaps stale or unused locks within the Backend. It is used by
	// the cilium-operator and is not invoked by cilium-agent. Returns
	// a map of locks currently being held in the KVStore including the ones
	// that failed to be GCed.
	// Note: not all Backend implementations rely on this, such as the kvstore
	// backends, and may use leases to expire keys.
	RunLocksGC(ctx context.Context, staleKeysPrevRound map[string]kvstore.Value) (map[string]kvstore.Value, error)
}

// NewAllocator creates a new Allocator. Any type can be used as key as long as
// the type implements the AllocatorKey interface. A variable of the type has
// to be passed into NewAllocator() to make the type known.  The specified base
// path is used to prefix all keys in the kvstore. The provided path must be
// unique.
//
// The allocator can be configured by passing in additional options:
//   - WithEvents() - enable Events channel
//   - WithMin(id) - minimum ID to allocate (default: 1)
//   - WithMax(id) - maximum ID to allocate (default max(uint64))
//
// After creation, IDs can be allocated with Allocate() and released with
// Release()
func NewAllocator(rootLogger *slog.Logger, typ AllocatorKey, backend Backend, opts ...AllocatorOption) (*Allocator, error) {
	a := &Allocator{
		logger:       rootLogger.With(subsysLogAttr...),
		keyType:      typ,
		backend:      backend,
		min:          idpool.ID(1),
		max:          idpool.ID(^uint64(0)),
		localKeys:    newLocalKeys(rootLogger),
		stopGC:       make(chan struct{}),
		remoteCaches: map[string]*remoteCache{},
		backoffTemplate: backoff.Exponential{
			Logger: rootLogger.With(subsysLogAttr...),
			Min:    time.Duration(20) * time.Millisecond,
			Factor: 2.0,
		},
		maxAllocAttempts: defaultMaxAllocAttempts,
		syncInterval:     DefaultSyncInterval,
	}

	for _, fn := range opts {
		fn(a)
	}

	a.mainCache = newCache(a)

	if a.min < 1 {
		return nil, errors.New("minimum ID must be >= 1")
	}

	if a.max <= a.min {
		return nil, fmt.Errorf("maximum ID must be greater than minimum ID: configured max %v, min %v", a.max, a.min)
	}

	a.idPool = idpool.NewIDPool(a.min, a.max)

	if !a.disableAutostart {
		a.start()
	}

	return a, nil
}

func (a *Allocator) start() {
	a.initialListDone = a.mainCache.start()
	if !a.disableGC {
		go func() {
			select {
			case <-a.initialListDone:
			case <-time.After(option.Config.AllocatorListTimeout):
				logging.Fatal(a.logger, "Timeout while waiting for initial allocator state")
			}
			a.startLocalKeySync()
		}()
	}
}

// WithBackend sets this allocator to use backend. It is expected to be used at
// initialization.
func WithBackend(backend Backend) AllocatorOption {
	return func(a *Allocator) {
		a.backend = backend
	}
}

// WithEvents enables receiving of events.
//
// CAUTION: When using this function. The provided channel must be continuously
// read while NewAllocator() is being called to ensure that the channel does
// not block indefinitely while NewAllocator() emits events on it while
// populating the initial cache.
func WithEvents(events AllocatorEventSendChan) AllocatorOption {
	return func(a *Allocator) { a.events = events }
}

// WithMin sets the minimum identifier to be allocated
func WithMin(id idpool.ID) AllocatorOption {
	return func(a *Allocator) { a.min = id }
}

// WithMax sets the maximum identifier to be allocated
func WithMax(id idpool.ID) AllocatorOption {
	return func(a *Allocator) { a.max = id }
}

// WithPrefixMask sets the prefix used for all ID allocations. If set, the mask
// will be ORed to all selected IDs prior to allocation. It is the
// responsibility of the caller to ensure that the mask is not conflicting with
// min..max.
func WithPrefixMask(mask idpool.ID) AllocatorOption {
	return func(a *Allocator) { a.prefixMask = mask }
}

// WithMasterKeyProtection will watch for delete events on master keys and
// re-created them if local usage suggests that the key is still in use
func WithMasterKeyProtection() AllocatorOption {
	return func(a *Allocator) { a.enableMasterKeyProtection = true }
}

// WithOperatorIDManagement enables the mode with cilium-operator managing
// Cilium Identities.
func WithOperatorIDManagement() AllocatorOption {
	return func(a *Allocator) { a.operatorIDManagement = true }
}

// WithMaxAllocAttempts sets the maxAllocAttempts. If not set, new Allocator
// will use defaultMaxAllocAttempts.
func WithMaxAllocAttempts(maxAttempts int) AllocatorOption {
	return func(a *Allocator) { a.maxAllocAttempts = maxAttempts }
}

// WithoutGC disables the use of the garbage collector
func WithoutGC() AllocatorOption {
	return func(a *Allocator) { a.disableGC = true }
}

// WithoutAutostart prevents starting the allocator when it is initialized
func WithoutAutostart() AllocatorOption {
	return func(a *Allocator) { a.disableAutostart = true }
}

// WithSyncInterval configures the interval for local keys refresh.
func WithSyncInterval(interval time.Duration) AllocatorOption {
	return func(a *Allocator) { a.syncInterval = interval }
}

// WithCacheValidator registers a validator triggered for each identity
// notification event to filter out invalid IDs and keys.
func WithCacheValidator(validator CacheValidator) AllocatorOption {
	return func(a *Allocator) { a.cacheValidators = append(a.cacheValidators, validator) }
}

// GetEvents returns the events channel given to the allocator when
// constructed.
// Note: This channel is not owned by the allocator!
func (a *Allocator) GetEvents() AllocatorEventSendChan {
	return a.events
}

// Delete deletes an allocator and stops the garbage collector
func (a *Allocator) Delete() {
	close(a.stopGC)
	a.mainCache.stop()
}

// WaitForInitialSync waits until the initial sync is complete
func (a *Allocator) WaitForInitialSync(ctx context.Context) error {
	select {
	case <-a.initialListDone:
	case <-ctx.Done():
		return fmt.Errorf("identity sync was cancelled: %w", ctx.Err())
	}

	return nil
}

// RangeFunc is the function called by RangeCache
type RangeFunc func(idpool.ID, AllocatorKey)

// ForeachCache iterates over the allocator cache and calls RangeFunc on each
// cached entry
func (a *Allocator) ForeachCache(cb RangeFunc) {
	a.mainCache.foreach(cb)

	a.remoteCachesMutex.RLock()
	for _, rc := range a.remoteCaches {
		rc.cache.foreach(cb)
	}
	a.remoteCachesMutex.RUnlock()
}

// selectAvailableID selects an available ID.
// Returns a triple of the selected ID ORed with prefixMask, the ID string and
// the originally selected ID.
func (a *Allocator) selectAvailableID() (idpool.ID, string, idpool.ID) {
	if id := a.idPool.LeaseAvailableID(); id != idpool.NoID {
		unmaskedID := id
		id |= a.prefixMask
		return id, id.String(), unmaskedID
	}

	return 0, "", 0
}

// AllocatorKey is the interface to implement in order for a type to be used as
// key for the allocator. The key's data is assumed to be a collection of
// pkg/label.Label, and the functions reflect this somewhat.
type AllocatorKey interface {
	fmt.Stringer

	// GetKey returns the canonical string representation of the key
	GetKey() string

	// PutKey stores the information in v into the key. This is the inverse
	// operation to GetKey
	PutKey(v string) AllocatorKey

	// GetAsMap returns the key as a collection of "labels" with a key and value.
	// This is the inverse operation to PutKeyFromMap.
	GetAsMap() map[string]string

	// PutKeyFromMap stores the labels in v into the key to be used later. This
	// is the inverse operation to GetAsMap.
	PutKeyFromMap(v map[string]string) AllocatorKey

	// PutValue puts metadata inside the global identity for the given 'key' with
	// the given 'value'.
	PutValue(key any, value any) AllocatorKey

	// Value returns the value stored in the metadata map.
	Value(key any) any
}

// Return values:
//  1. allocated ID
//  2. whether the ID is newly allocated from kvstore
//  3. whether this is the first owner that holds a reference to the key in
//     localkeys store
//  4. error in case of failure
func (a *Allocator) lockedAllocate(ctx context.Context, key AllocatorKey) (idpool.ID, bool, bool, error) {
	var firstUse bool

	kvstore.Trace(a.logger, "Allocating key in kvstore", fieldKey, key)

	k := key.GetKey()
	lock, err := a.backend.Lock(ctx, key)
	if err != nil {
		return 0, false, false, err
	}

	defer lock.Unlock(context.Background())

	// fetch first key that matches /value/<key>
	value, err := a.GetIfLocked(ctx, key, lock)
	if err != nil {
		return 0, false, false, err
	}

	kvstore.Trace(a.logger, "kvstore state is: ", fieldID, value)

	a.slaveKeysMutex.Lock()
	defer a.slaveKeysMutex.Unlock()

	// We shouldn't assume the fact the master key does not exist in the kvstore
	// that localKeys does not have it. The KVStore might have lost all of its
	// data but the local agent still holds a reference for the given master key.
	if value == 0 {
		value = a.localKeys.lookupKey(k)
		if value != 0 {
			// re-create master key
			if err := a.backend.UpdateKeyIfLocked(ctx, value, key, true, lock); err != nil {
				return 0, false, false, fmt.Errorf("unable to re-create missing master key '%s': %s while allocating ID: %w", key, value, err)
			}
		}
	} else {
		_, firstUse, err = a.localKeys.allocate(k, key, value)
		if err != nil {
			return 0, false, false, fmt.Errorf("unable to reserve local key '%s': %w", k, err)
		}

		if firstUse {
			a.logger.Debug("Reserved new local key", logfields.Key, k)
		} else {
			a.logger.Debug("Reusing existing local key", logfields.Key, k)
		}
	}

	if value != 0 {
		a.logger.Debug("Reusing existing global key", logfields.Key, k)

		if err = a.backend.AcquireReference(ctx, value, key, lock); err != nil {
			a.localKeys.release(k)
			return 0, false, false, fmt.Errorf("unable to create secondary key '%s': %w", k, err)
		}

		// mark the key as verified in the local cache
		if err := a.localKeys.verify(k); err != nil {
			a.logger.Error("BUG: Unable to verify local key", logfields.Error, err)
		}

		return value, false, firstUse, nil
	}

	a.logger.Debug("Allocating new master ID", logfields.Key, k)
	id, strID, unmaskedID := a.selectAvailableID()
	if id == 0 {
		return 0, false, false, fmt.Errorf("no more available IDs in configured space")
	}

	kvstore.Trace(a.logger, "Selected available key ID", fieldID, id)

	releaseKeyAndID := func() {
		a.localKeys.release(k)
		a.idPool.Release(unmaskedID) // This returns this ID to be re-used for other keys
	}

	oldID, firstUse, err := a.localKeys.allocate(k, key, id)
	if err != nil {
		a.idPool.Release(unmaskedID)
		return 0, false, false, fmt.Errorf("unable to reserve local key '%s': %w", k, err)
	}

	// Another local writer beat us to allocating an ID for the same key,
	// start over
	if id != oldID {
		releaseKeyAndID()
		return 0, false, false, fmt.Errorf("another writer has allocated key %s", k)
	}

	// Check that this key has not been allocated in the cluster during our
	// operation here
	value, err = a.GetNoCache(ctx, key)
	if err != nil {
		releaseKeyAndID()
		return 0, false, false, err
	}
	if value != 0 {
		releaseKeyAndID()
		return 0, false, false, fmt.Errorf("Found master key after proceeding with new allocation for %s", k)
	}

	// Assigned to 'key' from 'key2' since in case of an error, we don't replace
	// the original 'key' variable with 'nil'.
	key2 := key
	key, err = a.backend.AllocateIDIfLocked(ctx, id, key2, lock)
	if err != nil {
		// Creation failed. Another agent most likely beat us to allocting this
		// ID, retry.
		releaseKeyAndID()
		return 0, false, false, fmt.Errorf("unable to allocate ID %s for key %s: %w", strID, key2, err)
	}

	// Notify pool that leased ID is now in-use.
	a.idPool.Use(unmaskedID)

	if err = a.backend.AcquireReference(ctx, id, key, lock); err != nil {
		// We will leak the master key here as the key has already been
		// exposed and may be in use by other nodes. The garbage
		// collector will release it again.
		releaseKeyAndID()
		return 0, false, false, fmt.Errorf("secondary key creation failed '%s': %w", k, err)
	}

	// mark the key as verified in the local cache
	if err := a.localKeys.verify(k); err != nil {
		a.logger.Error("BUG: Unable to verify local key", logfields.Error, err)
	}

	a.logger.Debug("Allocated new global key", logfields.Key, k)

	return id, true, firstUse, nil
}

// Allocate will retrieve the ID for the provided key. If no ID has been
// allocated for this key yet, a key will be allocated. If allocation fails,
// most likely due to a parallel allocation of the same ID by another user,
// allocation is re-attempted for maxAllocAttempts times.
//
// Return values:
//  1. allocated ID
//  2. whether the ID is newly allocated from kvstore
//  3. whether this is the first owner that holds a reference to the key in
//     localkeys store
//  4. error in case of failure
func (a *Allocator) Allocate(ctx context.Context, key AllocatorKey) (idpool.ID, bool, bool, error) {
	var (
		err      error
		value    idpool.ID
		isNew    bool
		firstUse bool
	)

	a.logger.Debug("Allocating key", logfields.Key, key)

	select {
	case <-a.initialListDone:
	case <-ctx.Done():
		return 0, false, false, fmt.Errorf("allocation was cancelled while waiting for initial key list to be received: %w", ctx.Err())
	}

	if a.operatorIDManagement {
		id, err := a.GetWithRetry(ctx, key)
		// The second and third return values are always false when
		// operatorIDManagement is enabled because cilium-operator manages security
		// IDs, and they are never newly allocated or require holding a reference to
		// a key.
		return id, false, false, err
	}

	kvstore.Trace(a.logger, "Allocating from kvstore", fieldKey, key)

	// make a copy of the template and customize it
	boff := a.backoffTemplate
	boff.Name = key.String()

	for attempt := range a.maxAllocAttempts {
		// Check our list of local keys already in use and increment the
		// refcnt. The returned key must be released afterwards. No kvstore
		// operation was performed for this allocation.
		// We also do this on every loop as a different Allocate call might have
		// allocated the key while we are attempting to allocate in this
		// execution thread. It does not hurt to check if localKeys contains a
		// reference for the key that we are attempting to allocate.
		if val := a.localKeys.use(key.GetKey()); val != idpool.NoID {
			kvstore.Trace(a.logger, "Reusing local id",
				fieldID, val,
				fieldKey, key,
			)
			a.mainCache.insert(key, val)
			return val, false, false, nil
		}

		// FIXME: Add non-locking variant
		value, isNew, firstUse, err = a.lockedAllocate(ctx, key)
		if err == nil {
			a.mainCache.insert(key, value)
			a.logger.Debug("Allocated key",
				logfields.Key, key,
				logfields.ID, value,
			)
			return value, isNew, firstUse, nil
		}

		select {
		case <-ctx.Done():
			a.logger.Warn("Ongoing key allocation has been cancelled",
				logfields.Error, ctx.Err(),
				logfields.Key, key,
				logfields.Attempt, attempt,
			)
			return 0, false, false, fmt.Errorf("key allocation cancelled: %w", ctx.Err())
		default:
			a.logger.Warn("Key allocation attempt failed",
				logfields.Error, err,
				logfields.Key, key,
				logfields.Attempt, attempt,
			)
		}

		kvstore.Trace(a.logger, "Allocation attempt failed",
			fieldKey, key,
			logfields.Attempt, attempt,
		)

		if waitErr := boff.Wait(ctx); waitErr != nil {
			return 0, false, false, waitErr
		}
	}

	return 0, false, false, err
}

func (a *Allocator) GetWithRetry(ctx context.Context, key AllocatorKey) (idpool.ID, error) {
	getID := func() (idpool.ID, error) {
		id, err := a.Get(ctx, key)
		if err != nil {
			return idpool.NoID, err
		}

		if id == idpool.NoID {
			return idpool.NoID, fmt.Errorf("security identity not found for key %s", key.String())
		}

		return id, nil
	}

	// Make a copy of the template and customize it.
	boff := a.backoffTemplate
	boff.Name = key.String()

	var id idpool.ID
	var err error

	for attempt := range a.maxAllocAttempts {
		id, err = getID()
		if err == nil {
			return id, nil
		}

		select {
		case <-ctx.Done():
			a.logger.Warn("Ongoing key allocation has been cancelled",
				logfields.Error, ctx.Err(),
				logfields.Key, key,
				logfields.Attempt, attempt,
			)
			return idpool.NoID, fmt.Errorf("key allocation cancelled: %w", ctx.Err())
		default:
			a.logger.Debug("CiliumIdentity not yet created by cilium-operator, retrying...",
				logfields.Error, err,
				logfields.Key, key,
				logfields.Attempt, attempt,
			)
		}

		if waitErr := boff.Wait(ctx); waitErr != nil {
			a.logger.Warn("timed out waiting for cilium-operator to allocate CiliumIdentity",
				logfields.Key, key,
				logfields.Attempt, attempt,
			)
			return idpool.NoID, fmt.Errorf("timed out waiting for cilium-operator to allocate CiliumIdentity for key %v, error: %w", key.GetKey(), waitErr)
		}
	}

	return idpool.NoID, err
}

// GetIfLocked returns the ID which is allocated to a key. Returns an ID of NoID if no ID
// has been allocated to this key yet if the client is still holding the given
// lock.
func (a *Allocator) GetIfLocked(ctx context.Context, key AllocatorKey, lock kvstore.KVLocker) (idpool.ID, error) {
	if id := a.mainCache.get(key.GetKey()); id != idpool.NoID {
		return id, nil
	}

	return a.backend.GetIfLocked(ctx, key, lock)
}

// Get returns the ID which is allocated to a key. Returns an ID of NoID if no ID
// has been allocated to this key yet.
func (a *Allocator) Get(ctx context.Context, key AllocatorKey) (idpool.ID, error) {
	if id := a.mainCache.get(key.GetKey()); id != idpool.NoID {
		return id, nil
	}

	return a.GetNoCache(ctx, key)
}

// GetNoCache returns the ID which is allocated to a key in the kvstore,
// bypassing the local copy of allocated keys.
func (a *Allocator) GetNoCache(ctx context.Context, key AllocatorKey) (idpool.ID, error) {
	return a.backend.Get(ctx, key)
}

// GetByID returns the key associated with an ID. Returns nil if no key is
// associated with the ID.
func (a *Allocator) GetByID(ctx context.Context, id idpool.ID) (AllocatorKey, error) {
	if key := a.mainCache.getByID(id); key != nil {
		return key, nil
	}

	return a.backend.GetByID(ctx, id)
}

// GetIncludeRemoteCaches returns the ID which is allocated to a key. Includes the
// caches of watched remote kvstores in the query. Returns an ID of NoID if no
// ID has been allocated in any remote kvstore to this key yet.
func (a *Allocator) GetIncludeRemoteCaches(ctx context.Context, key AllocatorKey) (idpool.ID, error) {
	// check main cache first
	if id := a.mainCache.get(key.GetKey()); id != idpool.NoID {
		return id, nil
	}

	// check remote caches
	a.remoteCachesMutex.RLock()
	for _, rc := range a.remoteCaches {
		if id := rc.cache.get(key.GetKey()); id != idpool.NoID {
			a.remoteCachesMutex.RUnlock()
			return id, nil
		}
	}
	a.remoteCachesMutex.RUnlock()

	// check main backend
	if id, err := a.backend.Get(ctx, key); id != idpool.NoID || err != nil {
		return id, err
	}

	// we skip checking remote backends explicitly here, to avoid
	// accidentally overloading them in case of lookups for invalid identities

	return idpool.NoID, nil
}

// GetByIDIncludeRemoteCaches returns the key associated with an ID. Includes
// the caches of watched remote kvstores in the query.
// Returns nil if no key is associated with the ID.
func (a *Allocator) GetByIDIncludeRemoteCaches(ctx context.Context, id idpool.ID) (AllocatorKey, error) {
	// check main cache first
	if key := a.mainCache.getByID(id); key != nil {
		return key, nil
	}

	// check remote caches
	a.remoteCachesMutex.RLock()
	for _, rc := range a.remoteCaches {
		if key := rc.cache.getByID(id); key != nil {
			a.remoteCachesMutex.RUnlock()
			return key, nil
		}
	}
	a.remoteCachesMutex.RUnlock()

	// check main backend
	if key, err := a.backend.GetByID(ctx, id); key != nil || err != nil {
		return key, err
	}

	// we skip checking remote backends explicitly here, to avoid
	// accidentally overloading them in case of lookups for invalid identities

	return nil, nil
}

// Release releases the use of an ID associated with the provided key. After
// the last user has released the ID, the key is removed in the KVstore and
// the returned lastUse value is true.
func (a *Allocator) Release(ctx context.Context, key AllocatorKey) (lastUse bool, err error) {
	if a.operatorIDManagement {
		a.logger.Debug("Skipping key release when cilium-operator ID management is enabled", logfields.Key, key)
		return false, nil
	}

	a.logger.Debug("Releasing key", logfields.Key, key)

	select {
	case <-a.initialListDone:
	case <-ctx.Done():
		return false, fmt.Errorf("release was cancelled while waiting for initial key list to be received: %w", ctx.Err())
	}

	k := key.GetKey()

	a.slaveKeysMutex.Lock()
	defer a.slaveKeysMutex.Unlock()

	// release the key locally, if it was the last use, remove the node
	// specific value key to remove the global reference mark
	var id idpool.ID
	lastUse, id, err = a.localKeys.release(k)
	if err != nil {
		return lastUse, err
	}
	if lastUse {
		// Since in CRD mode we don't have a way to map which identity is being
		// used by a node, we need to also pass the ID to the release function.
		// This allows the CRD store to find the right identity by its ID and
		// remove the node reference on that identity.
		a.backend.Release(ctx, id, key)
	}

	return lastUse, err
}

// RunGC scans the kvstore for unused master keys and removes them
func (a *Allocator) RunGC(ctx context.Context, rateLimit *rate.Limiter, staleKeysPrevRound map[string]uint64) (map[string]uint64, *GCStats, error) {
	return a.backend.RunGC(ctx, rateLimit, staleKeysPrevRound, a.min, a.max)
}

// RunLocksGC scans the kvstore for stale locks and removes them
func (a *Allocator) RunLocksGC(ctx context.Context, staleLocksPrevRound map[string]kvstore.Value) (map[string]kvstore.Value, error) {
	return a.backend.RunLocksGC(ctx, staleLocksPrevRound)
}

// DeleteAllKeys will delete all keys. It is expected to be used in tests.
func (a *Allocator) DeleteAllKeys() {
	a.backend.DeleteAllKeys(context.TODO())
}

// syncLocalKeys checks the kvstore and verifies that a master key exists for
// all locally used allocations. This will restore master keys if deleted for
// some reason.
func (a *Allocator) syncLocalKeys() {
	// Create a local copy of all local allocations to not require to hold
	// any locks while performing kvstore operations. Local use can
	// disappear while we perform the sync. For master keys this is fine as
	// the garbage collector will remove it again. However, for slave keys, they
	// will continue to exist until the kvstore lease expires after the agent is restarted.
	// To ensure slave keys are not leaked, we do an extra check after the upsert,
	// to ensure the key is still in use. If it's not in use, we grab the slave key mutex
	// and hold it until we have released the key knowing that no new usage has started during the operation.
	ids := a.localKeys.getVerifiedIDs()
	ctx := context.TODO()

	for id, key := range ids {
		a.syncLocalKey(ctx, id, key)
	}
}

func (a *Allocator) syncLocalKey(ctx context.Context, id idpool.ID, key AllocatorKey) {
	encodedKey := key.GetKey()
	if newId := a.localKeys.lookupKey(encodedKey); newId != id {
		return
	}
	err := a.backend.UpdateKey(ctx, id, key, false)
	if err != nil {
		a.logger.Warn(
			"Error updating key",
			logfields.Key, key,
			logfields.ID, id,
		)
	}

	// Check if the key is still in use locally. Given its expected it's still
	// in use in most cases, we avoid grabbing the slaveKeysMutex here to reduce lock contention.
	// If it is in use here, we know the slave key is not leaked, and we don't need to do any cleanup.
	if newId := a.localKeys.lookupKey(encodedKey); newId != idpool.NoID {
		return
	}

	a.slaveKeysMutex.Lock()
	defer a.slaveKeysMutex.Unlock()

	// Check once again that the slave key is unused locally before releasing it,
	// all while holding the slaveKeysMutex to ensure there are no concurrent allocations or releases.
	// If the key is still unused, it could mean that the slave key was upserted into the kvstore during "UpdateKey"
	// after it was previously released. If that is the case, we release it while holding the slaveKeysMutex.
	if newId := a.localKeys.lookupKey(encodedKey); newId == idpool.NoID {
		ctx, cancel := context.WithTimeout(ctx, backendOpTimeout)
		defer cancel()
		a.logger.Warn(
			"Releasing now unused key that was re-recreated",
			logfields.Key, key,
			logfields.ID, id,
		)
		err = a.backend.Release(ctx, id, key)
		if err != nil {
			a.logger.Warn(
				"Error releasing unused key",
				logfields.Error, err,
				logfields.Key, key,
				logfields.ID, id,
			)
		}
	}
}

func (a *Allocator) startLocalKeySync() {
	go func(a *Allocator) {
		for {
			a.syncLocalKeys()

			select {
			case <-a.stopGC:
				a.logger.Debug("Stopped master key sync routine")
				return
			case <-time.After(a.syncInterval):
			}
		}
	}(a)
}

// AllocatorEventChan is a channel to receive allocator events on
type AllocatorEventChan chan AllocatorEvent

// Send- and receive-only versions of the above.
type AllocatorEventRecvChan = <-chan AllocatorEvent
type AllocatorEventSendChan = chan<- AllocatorEvent

// AllocatorEvent is an event sent over AllocatorEventChan
type AllocatorEvent struct {
	// Typ is the type of event (upsert / delete)
	Typ AllocatorChangeKind

	// ID is the allocated ID
	ID idpool.ID

	// Key is the key associated with the ID
	Key AllocatorKey
}

// remoteCache represents the cache content of an additional kvstore managing
// identities. The contents are not directly accessible but will be merged into
// the ForeachCache() function.
type remoteCache struct {
	name string

	allocator *Allocator
	cache     *cache

	watchFunc func(ctx context.Context, remote *remoteCache, onSync func(context.Context))
}

type RemoteIDCache interface {
	NumEntries() int
	Synced() bool
	Watch(ctx context.Context, onSync func(context.Context))
}

func (a *Allocator) NewRemoteCache(remoteName string, remoteAlloc *Allocator) RemoteIDCache {
	return &remoteCache{
		name:      remoteName,
		allocator: remoteAlloc,
		cache:     &remoteAlloc.mainCache,

		watchFunc: a.watchRemoteKVStore,
	}
}

// watchRemoteKVStore starts watching an allocator base prefix the kvstore
// represents by the provided backend. A local cache of all identities of that
// kvstore will be maintained in the RemoteCache structure returned and will
// start being reported in the identities returned by the ForeachCache()
// function. RemoteName should be unique per logical "remote".
func (a *Allocator) watchRemoteKVStore(ctx context.Context, rc *remoteCache, onSync func(context.Context)) {
	scopedLog := a.logger.With(logfields.ClusterName, rc.name)
	scopedLog.Info("Starting remote kvstore watcher")

	rc.allocator.start()

	select {
	case <-ctx.Done():
		scopedLog.Debug("Context canceled before remote kvstore watcher synchronization completed: stale identities will now be drained")
		rc.close()

		a.remoteCachesMutex.RLock()
		old := a.remoteCaches[rc.name]
		a.remoteCachesMutex.RUnlock()

		if old != nil {
			old.cache.mutex.RLock()
			defer old.cache.mutex.RUnlock()
		}

		// Drain all entries that might have been received until now, and that
		// are not present in the current cache (if any). This ensures we do not
		// leak any stale identity, and at the same time we do not invalidate the
		// current state.
		rc.cache.drainIf(func(id idpool.ID) bool {
			if old == nil {
				return true
			}

			_, ok := old.cache.nextCache[id]
			return !ok
		})
		return

	case <-rc.cache.listDone:
		scopedLog.Info("Remote kvstore watcher successfully synchronized and registered")
	}

	a.remoteCachesMutex.Lock()
	old := a.remoteCaches[rc.name]
	a.remoteCaches[rc.name] = rc
	a.remoteCachesMutex.Unlock()

	if old != nil {
		// In case of reconnection, let's emit a deletion event for all stale identities
		// that are no longer present in the kvstore. We take the lock of the new cache
		// to ensure that we observe a stable state during this process (i.e., no keys
		// are added/removed in the meanwhile).
		scopedLog.Debug("Another kvstore watcher was already registered: deleting stale identities")
		rc.cache.mutex.RLock()
		old.cache.drainIf(func(id idpool.ID) bool {
			_, ok := rc.cache.nextCache[id]
			return !ok
		})
		rc.cache.mutex.RUnlock()
	}

	// Execute the on-sync callback handler.
	onSync(ctx)

	<-ctx.Done()
	rc.close()
	scopedLog.Info("Stopped remote kvstore watcher")
}

// RemoveRemoteKVStore removes any reference to a remote allocator / kvstore, emitting
// a deletion event for all previously known identities.
func (a *Allocator) RemoveRemoteKVStore(remoteName string) {
	a.remoteCachesMutex.Lock()
	old := a.remoteCaches[remoteName]
	delete(a.remoteCaches, remoteName)
	a.remoteCachesMutex.Unlock()

	if old != nil {
		old.cache.drain()
		a.logger.Info("Remote kvstore watcher unregistered", logfields.ClusterName, remoteName)
	}
}

// Watch starts watching the remote kvstore and synchronize the identities in
// the local cache. It blocks until the context is closed.
func (rc *remoteCache) Watch(ctx context.Context, onSync func(context.Context)) {
	rc.watchFunc(ctx, rc, onSync)
}

// NumEntries returns the number of entries in the remote cache
func (rc *remoteCache) NumEntries() int {
	if rc == nil {
		return 0
	}

	return rc.cache.numEntries()
}

// Synced returns whether the initial list of entries has been retrieved from
// the kvstore, and new events are currently being watched.
func (rc *remoteCache) Synced() bool {
	if rc == nil {
		return false
	}

	select {
	case <-rc.cache.ctx.Done():
		// The cache has been stopped.
		return false
	default:
		select {
		case <-rc.cache.listDone:
			return true
		default:
			return false
		}
	}
}

// close stops watching for identities in the kvstore associated with the
// remote cache.
func (rc *remoteCache) close() {
	rc.cache.allocator.Delete()
}

// Observe the identity changes. Conforms to stream.Observable.
// Replays the current state of the cache when subscribing.
func (a *Allocator) Observe(ctx context.Context, next func(AllocatorChange), complete func(error)) {
	a.mainCache.Observe(ctx, next, complete)
}
