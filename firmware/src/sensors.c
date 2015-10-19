#include <plat/inc/taggedPtr.h>
#include <cpu/inc/barrier.h>
#include <atomicBitset.h>
#include <sensors.h>
#include <atomic.h>
#include <stdio.h>
#include <slab.h>
#include <seos.h>

#define MAX_INTERNAL_EVENTS       32 //also used for external app sensors' setRate() calls
#define MAX_CLI_SENS_MATRIX_SZ    64 /* MAX(numClients * numSensors) */

#define SENSOR_RATE_OFF           0x00000000UL /* used in sensor state machine */
#define SENSOR_RATE_POWERING_ON   0xFFFFFFF0UL /* used in sensor state machine */
#define SENSOR_RATE_POWERING_OFF  0xFFFFFFF1UL /* used in sensor state machine */
#define SENSOR_RATE_FW_UPLOADING  0xFFFFFFF2UL /* used in sensor state machine */
#define SENSOR_RATE_IMPOSSIBLE    0xFFFFFFF3UL /* used in rate calc to indicate impossible combinations */
#define SENSOR_LATENCY_INVALID    0xFFFFFFFFFFFFFFFFULL

struct Sensor {
    const struct SensorInfo *si;
    uint32_t handle;         /* here 0 means invalid */
    uint64_t currentLatency; /* here 0 means no batching */
    uint32_t currentRate;    /* here 0 means off */
    TaggedPtr callInfo;      /* pointer to ops struct or app tid */
} __attribute__((packed));

struct SensorsInternalEvent {
    union {
        struct {
            uint32_t handle;
            uint32_t value1;
            uint64_t value2;
        };
        struct SensorSetRateEvent externalEvt;
    };
};

struct SensorsClientRequest {
    uint32_t handle;
    uint32_t clientId;
    uint64_t latency;
    uint32_t rate;
};

static struct Sensor mSensors[MAX_REGISTERED_SENSORS];
ATOMIC_BITSET_DECL(mSensorsUsed, MAX_REGISTERED_SENSORS, static);
static struct SlabAllocator *mInternalEvents;
static struct SlabAllocator *mCliSensMatrix;
static uint32_t mNextSensorHandle;




bool sensorsInit(void)
{
    atomicBitsetInit(mSensorsUsed, MAX_REGISTERED_SENSORS);

    mInternalEvents = slabAllocatorNew(sizeof(struct SensorsInternalEvent), 4, MAX_INTERNAL_EVENTS);
    if (!mInternalEvents)
        return false;

    mCliSensMatrix = slabAllocatorNew(sizeof(struct SensorsClientRequest), 4, MAX_CLI_SENS_MATRIX_SZ);
    if (mCliSensMatrix)
        return true;

    slabAllocatorDestroy(mInternalEvents);

    return false;
}

static struct Sensor* sensorFindByHandle(uint32_t handle)
{
    uint32_t i;

    for (i = 0; i < MAX_REGISTERED_SENSORS; i++)
        if (mSensors[i].handle == handle)
            return mSensors + i;

    return NULL;
}

static uint32_t sensorRegisterEx(const struct SensorInfo *si, TaggedPtr callInfo)
{
    int32_t idx = atomicBitsetFindClearAndSet(mSensorsUsed);
    struct Sensor *s;
    uint32_t handle;

    /* grab a slot */
    if (idx < 0)
        return 0;

    /* grab a handle */
    do {
        handle = atomicAdd(&mNextSensorHandle, 1);
    } while (!handle || sensorFindByHandle(handle)); /* this is safe since nobody else could have "JUST" taken this handle, we'll need to circle around 32bits before that happens */

    /* fill the struct in and mark it valid (by setting handle) */
    s = mSensors + idx;
    s->si = si;
    s->currentRate = SENSOR_RATE_OFF;
    s->currentLatency = SENSOR_LATENCY_INVALID;
    s->callInfo = callInfo;
    mem_reorder_barrier();
    s->handle = handle;

    return handle;
}

uint32_t sensorRegister(const struct SensorInfo *si, const struct SensorOps *ops)
{
    return sensorRegisterEx(si, taggedPtrMakeFromPtr(ops));
}

uint32_t sensorRegisterAsApp(const struct SensorInfo *si, uint32_t tid)
{
    return sensorRegisterEx(si, taggedPtrMakeFromUint(tid));
}

bool sensorUnregister(uint32_t handle)
{
    struct Sensor *s = sensorFindByHandle(handle);

    if (!s)
        return false;

    /* mark as invalid */
    s->handle = 0;
    mem_reorder_barrier();

    /* free struct */
    atomicBitsetClearBit(mSensorsUsed, s - mSensors);

    return true;
}

static bool sensorCallFuncPower(struct Sensor* s, bool on)
{
    if (taggedPtrIsPtr(s->callInfo))
        return ((const struct SensorOps*)taggedPtrToPtr(s->callInfo))->sensorPower(on);
    else
        return osEnqueuePrivateEvt(EVT_APP_SENSOR_POWER, (void*)(uintptr_t)on, NULL, taggedPtrToUint(s->callInfo));
}

static bool sensorCallFuncFwUpld(struct Sensor* s)
{
    if (taggedPtrIsPtr(s->callInfo))
        return ((const struct SensorOps*)taggedPtrToPtr(s->callInfo))->sensorFirmwareUpload();
    else
        return osEnqueuePrivateEvt(EVT_APP_SENSOR_FW_UPLD, NULL, NULL, taggedPtrToUint(s->callInfo));
}

static void sensorCallFuncSetRateEvtFreeF(void* event)
{
    slabAllocatorFree(mInternalEvents, event);
}

static bool sensorCallFuncSetRate(struct Sensor* s, uint32_t rate, uint64_t latency)
{
    if (taggedPtrIsPtr(s->callInfo))
        return ((const struct SensorOps*)taggedPtrToPtr(s->callInfo))->sensorSetRate(rate, latency);
    else {
        struct SensorsInternalEvent *evt = (struct SensorsInternalEvent*)slabAllocatorAlloc(mInternalEvents);

        if (!evt)
            return false;

        evt->externalEvt.latency = latency;
        evt->externalEvt.rate = rate;
        if (osEnqueuePrivateEvt(EVT_APP_SENSOR_SET_RATE, &evt->externalEvt, sensorCallFuncSetRateEvtFreeF, taggedPtrToUint(s->callInfo)))
            return true;

        slabAllocatorFree(mInternalEvents, evt);
        return false;
    }
}

static bool sensorCallFuncFlush(struct Sensor* s)
{
    if (taggedPtrIsPtr(s->callInfo))
        return ((const struct SensorOps*)taggedPtrToPtr(s->callInfo))->sensorFlush();
    else
        return osEnqueuePrivateEvt(EVT_APP_SENSOR_FLUSH, NULL, NULL, taggedPtrToUint(s->callInfo));
}

static bool sensorCallFuncTrigger(struct Sensor* s)
{
    if (taggedPtrIsPtr(s->callInfo))
        return ((const struct SensorOps*)taggedPtrToPtr(s->callInfo))->sensorTriggerOndemand();
    else
        return osEnqueuePrivateEvt(EVT_APP_SENSOR_TRIGGER, NULL, NULL, taggedPtrToUint(s->callInfo));
}

static void sensorReconfig(struct Sensor* s, uint32_t newHwRate, uint64_t newHwLatency)
{
    if (s->currentRate == newHwRate && s->currentLatency == newHwLatency) {
        /* do nothing */
    }
    else if (s->currentRate == SENSOR_RATE_OFF) {
        /* if it was off or is off, tell it to come on */
        if (sensorCallFuncPower(s, true)) {
            s->currentRate = SENSOR_RATE_POWERING_ON;
            s->currentLatency = SENSOR_LATENCY_INVALID;
        }
    }
    else if (s->currentRate == SENSOR_RATE_POWERING_OFF) {
        /* if it was going to be off or is off, tell it to come back on */
        s->currentRate = SENSOR_RATE_POWERING_ON;
        s->currentLatency = SENSOR_LATENCY_INVALID;
    }
    else if (s->currentRate == SENSOR_RATE_POWERING_ON || s->currentRate == SENSOR_RATE_FW_UPLOADING) {
        /* if it is powering on - do nothing - all will be done for us */
    }
    else if (newHwRate > SENSOR_RATE_OFF || newHwLatency < SENSOR_LATENCY_INVALID) {
        /* simple rate change - > do it, there is nothing we can do if this fails, so we ignore the immediate errors :( */
        (void)sensorCallFuncSetRate(s, newHwRate, newHwLatency);
    }
    else {
        /* powering off */
        if (sensorCallFuncPower(s, false)) {
            s->currentRate = SENSOR_RATE_POWERING_OFF;
            s->currentLatency = SENSOR_LATENCY_INVALID;
        }
    }
}

static uint64_t sensorCalcHwLatency(struct Sensor* s)
{
    uint64_t smallestLatency = SENSOR_LATENCY_INVALID;
    uint32_t i;

    for (i = 0; i < MAX_CLI_SENS_MATRIX_SZ; i++) {
        struct SensorsClientRequest *req = slabAllocatorGetNth(mCliSensMatrix, i);

        /* we only care about this sensor's stuff */
        if (!req || req->handle != s->handle)
            continue;

        if (smallestLatency > req->latency)
            smallestLatency = req->latency;
    }

    return smallestLatency;
}

static uint32_t sensorCalcHwRate(struct Sensor* s, uint32_t extraReqedRate, uint32_t removedRate)
{
    bool haveUsers = false, haveOnChange = extraReqedRate == SENSOR_RATE_ONCHANGE;
    uint32_t highestReq = 0;
    uint32_t i;

    if (extraReqedRate) {
         haveUsers = true;
         highestReq = (extraReqedRate == SENSOR_RATE_ONDEMAND || extraReqedRate == SENSOR_RATE_ONCHANGE) ? 0 : extraReqedRate;
    }

    for (i = 0; i < MAX_CLI_SENS_MATRIX_SZ; i++) {
        struct SensorsClientRequest *req = slabAllocatorGetNth(mCliSensMatrix, i);

        /* we only care about this sensor's stuff */
        if (!req || req->handle != s->handle)
            continue;

        /* skip an instance of a removed rate if one was given */
        if (req->rate == removedRate) {
            removedRate = SENSOR_RATE_OFF;
            continue;
        }

        haveUsers = true;

        /* we can always do ondemand and if we see an on-change then we already checked and do allow it */
        if (req->rate == SENSOR_RATE_ONDEMAND)
            continue;
        if (req->rate == SENSOR_RATE_ONCHANGE) {
            haveOnChange = true;
            continue;
        }

        if (highestReq < req->rate)
            highestReq = req->rate;
    }

    if (!highestReq) {   /* no requests -> we can definitely do that */
        if (!haveUsers)
            return SENSOR_RATE_OFF;
        else if (haveOnChange)
            return SENSOR_RATE_ONCHANGE;
        else
            return SENSOR_RATE_ONDEMAND;
    }

    for (i = 0; s->si->supportedRates[i]; i++)
        if (s->si->supportedRates[i] >= highestReq)
            return s->si->supportedRates[i];

    return SENSOR_RATE_IMPOSSIBLE;
}

static void sensorInternalFwStateChanged(void *evtP)
{
    struct SensorsInternalEvent *evt = (struct SensorsInternalEvent*)evtP;
    struct Sensor* s = sensorFindByHandle(evt->handle);

    if (s) {

        if (!evt->value1) {                                       //we failed -> give up
            s->currentRate = SENSOR_RATE_POWERING_OFF;
            s->currentLatency = SENSOR_LATENCY_INVALID;
            sensorCallFuncPower(s, false);
        }
        else if (s->currentRate == SENSOR_RATE_FW_UPLOADING) {    //we're up
            s->currentRate = evt->value1;
            s->currentLatency = evt->value2;
            sensorReconfig(s, sensorCalcHwRate(s, 0, 0), sensorCalcHwLatency(s));
        }
        else if (s->currentRate == SENSOR_RATE_POWERING_OFF) {    //we need to power off
            sensorCallFuncPower(s, false);
        }
    }
    slabAllocatorFree(mInternalEvents, evt);
}

static void sensorInternalPowerStateChanged(void *evtP)
{
    struct SensorsInternalEvent *evt = (struct SensorsInternalEvent*)evtP;
    struct Sensor* s = sensorFindByHandle(evt->handle);

    if (s) {

        if (s->currentRate == SENSOR_RATE_POWERING_ON && evt->value1) {          //we're now on - upload firmware
            s->currentRate = SENSOR_RATE_FW_UPLOADING;
            s->currentLatency = SENSOR_LATENCY_INVALID;
            sensorCallFuncFwUpld(s);
        }
        else if (s->currentRate == SENSOR_RATE_POWERING_OFF && !evt->value1) {   //we're now off
            s->currentRate = SENSOR_RATE_OFF;
            s->currentLatency = SENSOR_LATENCY_INVALID;
        }
        else if (s->currentRate == SENSOR_RATE_POWERING_ON && !evt->value1) {    //we need to power back on
            sensorCallFuncPower(s, true);
        }
        else if (s->currentRate == SENSOR_RATE_POWERING_OFF && evt->value1) {    //we need to power back off
            sensorCallFuncPower(s, false);
        }
    }
    slabAllocatorFree(mInternalEvents, evt);
}

static void sensorInternalRateChanged(void *evtP)
{
    struct SensorsInternalEvent *evt = (struct SensorsInternalEvent*)evtP;
    struct Sensor* s = sensorFindByHandle(evt->handle);

    if (s) {
        s->currentRate = evt->value1;
        s->currentLatency = evt->value2;
    }
    slabAllocatorFree(mInternalEvents, evt);
}

bool sensorSignalInternalEvt(uint32_t handle, uint32_t intEvtNum, uint32_t value1, uint64_t value2)
{
    static const OsDeferCbkF internalEventCallbacks[] = {
        [SENSOR_INTERNAL_EVT_POWER_STATE_CHG] = sensorInternalPowerStateChanged,
        [SENSOR_INTERNAL_EVT_FW_STATE_CHG] = sensorInternalFwStateChanged,
        [SENSOR_INTERNAL_EVT_RATE_CHG] = sensorInternalRateChanged,
    };
    struct SensorsInternalEvent *evt = (struct SensorsInternalEvent*)slabAllocatorAlloc(mInternalEvents);

    if (!evt)
        return false;

    evt->handle = handle;
    evt->value1 = value1;
    evt->value2 = value2;

    if (osDefer(internalEventCallbacks[intEvtNum], evt))
        return true;

    slabAllocatorFree(mInternalEvents, evt);
    return false;
}

const struct SensorInfo* sensorFind(uint32_t sensorType, uint32_t idx, uint32_t *handleP)
{
    uint32_t i;

    for (i = 0; i < MAX_REGISTERED_SENSORS; i++) {
        if (mSensors[i].handle && mSensors[i].si->sensorType == sensorType && !idx--) {
            if (handleP)
                *handleP = mSensors[i].handle;
            return mSensors[i].si;
        }
    }

    return NULL;
}

static bool sensorAddRequestor(uint32_t sensorHandle, uint32_t clientId, uint32_t rate, uint64_t latency)
{
    struct SensorsClientRequest *req = slabAllocatorAlloc(mCliSensMatrix);

    if (!req)
        return false;

    req->handle = sensorHandle;
    req->clientId = clientId;
    mem_reorder_barrier();
    req->rate = rate;
    req->latency = latency;

    return true;
}

static bool sensorGetCurRequestorRate(uint32_t sensorHandle, uint32_t clientId, uint32_t *rateP, uint64_t *latencyP)
{
    uint32_t i;

    for (i = 0; i < MAX_CLI_SENS_MATRIX_SZ; i++) {
        struct SensorsClientRequest *req = slabAllocatorGetNth(mCliSensMatrix, i);

        if (req && req->handle == sensorHandle && req->clientId == clientId) {
            if (rateP) {
                *rateP = req->rate;
                *latencyP = req->latency;
            }
            return true;
        }
    }

    return false;
}

static bool sensorAmendRequestor(uint32_t sensorHandle, uint32_t clientId, uint32_t newRate, uint64_t newLatency)
{
    uint32_t i;

    for (i = 0; i < MAX_CLI_SENS_MATRIX_SZ; i++) {
        struct SensorsClientRequest *req = slabAllocatorGetNth(mCliSensMatrix, i);

        if (req && req->handle == sensorHandle && req->clientId == clientId) {
            req->rate = newRate;
            req->latency = newLatency;
            return true;
        }
    }

    return false;
}

static bool sensorDeleteRequestor(uint32_t sensorHandle, uint32_t clientId)
{
    uint32_t i;

    for (i = 0; i < MAX_CLI_SENS_MATRIX_SZ; i++) {
        struct SensorsClientRequest *req = slabAllocatorGetNth(mCliSensMatrix, i);

        if (req && req->handle == sensorHandle && req->clientId == clientId) {
            req->rate = SENSOR_RATE_OFF;
            req->latency = SENSOR_LATENCY_INVALID;
            mem_reorder_barrier();
            slabAllocatorFree(mCliSensMatrix, req);
            return true;
        }
    }

    return false;
}

bool sensorRequest(uint32_t clientId, uint32_t sensorHandle, uint32_t rate, uint64_t latency)
{
    struct Sensor* s = sensorFindByHandle(sensorHandle);
    uint32_t newSensorRate;

    if (!s)
        return false;

    /* verify the rate is possible */
    newSensorRate = sensorCalcHwRate(s, rate, 0);
    if (newSensorRate == SENSOR_RATE_IMPOSSIBLE)
        return false;

    /* record the request */
    if (!sensorAddRequestor(sensorHandle, clientId, rate, latency))
        return false;

    /* update actual sensor if needed */
    sensorReconfig(s, newSensorRate, sensorCalcHwLatency(s));
    return true;
}

bool sensorRequestRateChange(uint32_t clientId, uint32_t sensorHandle, uint32_t newRate, uint64_t newLatency)
{
    struct Sensor* s = sensorFindByHandle(sensorHandle);
    uint32_t oldRate, newSensorRate;
    uint64_t oldLatency;

    if (!s)
        return false;


    /* get current rate */
    if (!sensorGetCurRequestorRate(sensorHandle, clientId, &oldRate, &oldLatency))
        return false;

    /* verify the new rate is possible given all othe rongoing requests */
    newSensorRate = sensorCalcHwRate(s, newRate, oldRate);
    if (newSensorRate == SENSOR_RATE_IMPOSSIBLE)
        return false;

    /* record the request */
    if (!sensorAmendRequestor(sensorHandle, clientId, newRate, newLatency))
        return false;

    /* update actual sensor if needed */
    sensorReconfig(s, newSensorRate, sensorCalcHwLatency(s));
    return true;
}

bool sensorRelease(uint32_t clientId, uint32_t sensorHandle)
{
    struct Sensor* s = sensorFindByHandle(sensorHandle);
    if (!s)
        return false;

    /* record the request */
    if (!sensorDeleteRequestor(sensorHandle, clientId))
        return false;

    /* update actual sensor if needed */
    sensorReconfig(s, sensorCalcHwRate(s, 0, 0), sensorCalcHwLatency(s));
    return true;
}

bool sensorTriggerOndemand(uint32_t clientId, uint32_t sensorHandle)
{
    struct Sensor* s = sensorFindByHandle(sensorHandle);
    uint32_t i;

    if (!s)
        return false;

    for (i = 0; i < MAX_CLI_SENS_MATRIX_SZ; i++) {
        struct SensorsClientRequest *req = slabAllocatorGetNth(mCliSensMatrix, i);

        if (req && req->handle == sensorHandle && req->clientId == clientId)
            return sensorCallFuncTrigger(s);
    }

    // not found -> do not report
    return false;
}

bool sensorFlush(uint32_t sensorHandle)
{
    struct Sensor* s = sensorFindByHandle(sensorHandle);

    if (!s)
        return false;

    return sensorCallFuncFlush(s);
}

uint32_t sensorGetCurRate(uint32_t sensorHandle)
{
    struct Sensor* s = sensorFindByHandle(sensorHandle);

    return s ? s->currentRate : SENSOR_RATE_OFF;
}

uint64_t sensorGetCurLatency(uint32_t sensorHandle)
{
    struct Sensor* s = sensorFindByHandle(sensorHandle);

    return s ? s->currentLatency : SENSOR_LATENCY_INVALID;
}
