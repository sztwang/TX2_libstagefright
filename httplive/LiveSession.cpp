/*
* Copyright (C) 2014 MediaTek Inc.
* Modification based on code covered by the mentioned copyright
* and/or permission notice(s).
*/
/*
 * Copyright (C) 2010 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//#define LOG_NDEBUG 0
#define LOG_TAG "LiveSession"
#include <utils/Log.h>

#include "LiveSession.h"

#include "M3UParser.h"
#include "PlaylistFetcher.h"

#include "include/HTTPBase.h"
#include "mpeg2ts/AnotherPacketSource.h"

#include <cutils/properties.h>
#include <media/IMediaHTTPConnection.h>
#include <media/IMediaHTTPService.h>
#include <media/stagefright/foundation/hexdump.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/DataSource.h>
#include <media/stagefright/FileSource.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/MediaHTTP.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/Utils.h>

#include <utils/Mutex.h>

#include <ctype.h>
#include <inttypes.h>
#include <openssl/aes.h>
#include <openssl/md5.h>

#ifdef MTK_AOSP_ENHANCEMENT
#define LIVESESSION_USE_XLOG
#ifdef LIVESESSION_USE_XLOG
#include <cutils/xlog.h>
#undef ALOGE
#undef ALOGW
#undef ALOGI
#undef ALOGD
#undef ALOGV
#define ALOGE XLOGE
#define ALOGW XLOGW
#define ALOGI XLOGI
#define ALOGD XLOGD
#define ALOGV XLOGV
#endif
#endif

namespace android {

#ifdef MTK_AOSP_ENHANCEMENT
//max support 10s of 2Mbps.
const size_t LiveSession::kBandwidthHistoryBytes = 2500 * 1024;
#else
// Number of recently-read bytes to use for bandwidth estimation
const size_t LiveSession::kBandwidthHistoryBytes = 200 * 1024;
#endif

LiveSession::LiveSession(
        const sp<AMessage> &notify, uint32_t flags,
        const sp<IMediaHTTPService> &httpService)
    : mNotify(notify),
      mFlags(flags),
      mHTTPService(httpService),
      mInPreparationPhase(true),
      mHTTPDataSource(new MediaHTTP(mHTTPService->makeHTTPConnection())),
      mCurBandwidthIndex(-1),
      mStreamMask(0),
      mNewStreamMask(0),
      mSwapMask(0),
      mCheckBandwidthGeneration(0),
      mSwitchGeneration(0),
      mSubtitleGeneration(0),
      mLastDequeuedTimeUs(0ll),
      mRealTimeBaseUs(0ll),
      mReconfigurationInProgress(false),
      mSwitchInProgress(false),
      mDisconnectReplyID(0),
      mSeekReplyID(0),
      mFirstTimeUsValid(false),
      mFirstTimeUs(0),
      mLastSeekTimeUs(0) {
#ifdef MTK_AOSP_ENHANCEMENT
    mForceDisconnect = false;
    mIsBuffering = false;
    //add new looper for playlistfetcher for fetch file.
    mPlaylistfetcherLooper = new ALooper;
    mPlaylistfetcherLooper->setName("playlistfetcher");
    mPlaylistfetcherLooper->start();

    mReadSucessCount[kAudioIndex] = 0;
    mReadSucessCount[kVideoIndex] = 0;
    mReadSucessCount[kSubtitleIndex] = 0;

    ALOGI("LiveSession constructor");
#endif
    mStreams[kAudioIndex] = StreamItem("audio");
    mStreams[kVideoIndex] = StreamItem("video");
    mStreams[kSubtitleIndex] = StreamItem("subtitles");

    for (size_t i = 0; i < kMaxStreams; ++i) {
        mDiscontinuities.add(indexToType(i), new AnotherPacketSource(NULL /* meta */));
        mPacketSources.add(indexToType(i), new AnotherPacketSource(NULL /* meta */));
        mPacketSources2.add(indexToType(i), new AnotherPacketSource(NULL /* meta */));
        mBuffering[i] = false;
    }

    size_t numHistoryItems = kBandwidthHistoryBytes /
            PlaylistFetcher::kDownloadBlockSize + 1;
    if (numHistoryItems < 5) {
        numHistoryItems = 5;
    }
    mHTTPDataSource->setBandwidthHistorySize(numHistoryItems);
}

LiveSession::~LiveSession() {
#ifdef MTK_AOSP_ENHANCEMENT
    ALOGI("~LiveSession");
    mPlaylistfetcherLooper->stop();
    mPlaylistfetcherLooper.clear();
#endif
}

sp<ABuffer> LiveSession::createFormatChangeBuffer(bool swap) {
    ABuffer *discontinuity = new ABuffer(0);
    discontinuity->meta()->setInt32("discontinuity", ATSParser::DISCONTINUITY_FORMATCHANGE);
    discontinuity->meta()->setInt32("swapPacketSource", swap);
    discontinuity->meta()->setInt32("switchGeneration", mSwitchGeneration);
    discontinuity->meta()->setInt64("timeUs", -1);
    return discontinuity;
}

void LiveSession::swapPacketSource(StreamType stream) {
    sp<AnotherPacketSource> &aps = mPacketSources.editValueFor(stream);
    sp<AnotherPacketSource> &aps2 = mPacketSources2.editValueFor(stream);
    sp<AnotherPacketSource> tmp = aps;
    aps = aps2;
    aps2 = tmp;
    aps2->clear();
}

status_t LiveSession::dequeueAccessUnit(
        StreamType stream, sp<ABuffer> *accessUnit) {
#ifdef MTK_AOSP_ENHANCEMENT
    ALOGV("dequeueAccessUnit,stream(0x%x),mStreamMask(0x%x),mForceDisconnect(%d)",\
        stream,mStreamMask,mForceDisconnect);
#endif 
    if (!(mStreamMask & stream)) {
        // return -EWOULDBLOCK to avoid halting the decoder
        // when switching between audio/video and audio only.
        return -EWOULDBLOCK;
    }
#ifdef MTK_AOSP_ENHANCEMENT
    if(mForceDisconnect == true)
        return -EAGAIN;
#endif
    status_t finalResult;
    sp<AnotherPacketSource> discontinuityQueue  = mDiscontinuities.valueFor(stream);
    if (discontinuityQueue->hasBufferAvailable(&finalResult)) {
        discontinuityQueue->dequeueAccessUnit(accessUnit);
        // seeking, track switching
        sp<AMessage> extra;
        int64_t timeUs;
        if ((*accessUnit)->meta()->findMessage("extra", &extra)
                && extra != NULL
                && extra->findInt64("timeUs", &timeUs)) {
            // seeking only
            mLastSeekTimeUs = timeUs;
            mDiscontinuityOffsetTimesUs.clear();
            mDiscontinuityAbsStartTimesUs.clear();
#ifdef MTK_AOSP_ENHANCEMENT
            //when seek , can read  data tile duration >10s
            ssize_t idx = typeToIndex(stream);
            mBuffering[idx] = true;
            ALOGI("mDiscontinuities has seek discontinuity request at %" PRId64 " us set mBuffering[%d] to true",mLastSeekTimeUs, idx);
#endif
        }
        return INFO_DISCONTINUITY;
    }

    sp<AnotherPacketSource> packetSource = mPacketSources.valueFor(stream);

    ssize_t idx = typeToIndex(stream);
    if (!packetSource->hasBufferAvailable(&finalResult)) {
        if (finalResult == OK) {
#ifdef MTK_AOSP_ENHANCEMENT
	     if(mBuffering[idx] == false){
                ALOGD("packetSource has no buffer avaliable idx = %zd", idx);
	     }
#endif
            mBuffering[idx] = true;
            return -EAGAIN;
        } else {
            return finalResult;
        }
    }

    int32_t targetDuration = 0;
    sp<AMessage> meta = packetSource->getLatestEnqueuedMeta();
    if (meta != NULL) {
        meta->findInt32("targetDuration", &targetDuration);
    }

    int64_t targetDurationUs = targetDuration * 1000000ll;
    if (targetDurationUs == 0 ||
            targetDurationUs > PlaylistFetcher::kMinBufferedDurationUs) {
        // Fetchers limit buffering to
        // min(3 * targetDuration, kMinBufferedDurationUs)
        targetDurationUs = PlaylistFetcher::kMinBufferedDurationUs;
    }

    if (mBuffering[idx]) {
        if (mSwitchInProgress
                || packetSource->isFinished(0)
                || packetSource->getEstimatedDurationUs() > targetDurationUs) {
#ifdef MTK_AOSP_ENHANCEMENT
            ALOGD("mSwitchInProgress(%d),packetSource's duration = %lld",mSwitchInProgress, packetSource->getEstimatedDurationUs());
            if(!(mSwitchInProgress || packetSource->isFinished(0))){  //getEstimatedDurationUs > 10s
                uint32_t mask = mNewStreamMask & mStreamMask;
                sp<AnotherPacketSource> otherSource = NULL;
		  size_t otherstreamIdx = 0;
                if ((stream == STREAMTYPE_AUDIO) && (mask & STREAMTYPE_VIDEO) && (mPacketSources.valueFor(STREAMTYPE_VIDEO)->getFormat() != NULL)) {
		      otherSource = mPacketSources.valueFor(STREAMTYPE_VIDEO);
                    otherstreamIdx = kVideoIndex;
                } else if ((stream == STREAMTYPE_VIDEO) && (mask & STREAMTYPE_AUDIO) && (mPacketSources.valueFor(STREAMTYPE_AUDIO)->getFormat() != NULL)) {
                    otherSource = mPacketSources.valueFor(STREAMTYPE_AUDIO);
                    otherstreamIdx = kAudioIndex;	
                }
		  ALOGD("mask = %d, %d, %d", mask, mStreamMask, mNewStreamMask);
                //if another duration >targetDurationUs or  this duration > 30s, then set mBuffering[idx] = false;
                //avoid othersource has no media.
                if ((otherSource== NULL) || (otherSource->getEstimatedDurationUs() > targetDurationUs)
			                                  || (packetSource->getEstimatedDurationUs() > 30000000ll)){
                    mBuffering[idx] = false;
		      if(otherSource!= NULL){
                        mBuffering[otherstreamIdx] = false;
                        ALOGD("buffering up to 10s packsource->dur = %lld, othersource->dur = %lld",packetSource->getEstimatedDurationUs(), otherSource->getEstimatedDurationUs());
                    }else{
                        ALOGD("buffering up to 10s packsource->dur = %lld",packetSource->getEstimatedDurationUs());
                    }
                }
                
            }else{
                ALOGD("mSwitchInProgress(%d),packetSource is finished or buffering up to 10s",mSwitchInProgress);
		  mBuffering[idx] = false;
            }
#else
            mBuffering[idx] = false;
#endif
        }
    }

    if (mBuffering[idx]) {
        return -EAGAIN;
    }

    // wait for counterpart
    sp<AnotherPacketSource> otherSource;
    uint32_t mask = mNewStreamMask & mStreamMask;
    uint32_t fetchersMask  = 0;
    for (size_t i = 0; i < mFetcherInfos.size(); ++i) {
        uint32_t fetcherMask = mFetcherInfos.valueAt(i).mFetcher->getStreamTypeMask();
        fetchersMask |= fetcherMask;
    }
    mask &= fetchersMask;
    if (stream == STREAMTYPE_AUDIO && (mask & STREAMTYPE_VIDEO)) {
        otherSource = mPacketSources.valueFor(STREAMTYPE_VIDEO);
    } else if (stream == STREAMTYPE_VIDEO && (mask & STREAMTYPE_AUDIO)) {
        otherSource = mPacketSources.valueFor(STREAMTYPE_AUDIO);
    }
#ifdef MTK_AOSP_ENHANCEMENT
    // only judge other source timestamp.
    //if this source lastest timestamp little than othersource latest timestamp, then continue read
    if((otherSource != NULL) && (otherSource->getFormat() != NULL))
    {    
          size_t streamIdx = 0;
	   size_t otherstreamIdx = 0;
	   
          if (stream == STREAMTYPE_AUDIO && (mask & STREAMTYPE_VIDEO)) {
              otherSource = mPacketSources.valueFor(STREAMTYPE_VIDEO);
		streamIdx = kAudioIndex;
		otherstreamIdx = kVideoIndex;
          } else if (stream == STREAMTYPE_VIDEO && (mask & STREAMTYPE_AUDIO)) {
              otherSource = mPacketSources.valueFor(STREAMTYPE_AUDIO);
		streamIdx = kVideoIndex;
		otherstreamIdx = kAudioIndex;		
          }
           StreamItem& strm = mStreams[streamIdx];
    	   StreamItem& otherstrm = mStreams[otherstreamIdx];
            if (!otherSource->hasBufferAvailable(&finalResult)) {
               if(mReadSucessCount[idx] == 0)
	   	       ALOGE("idx = %zd duration = %lld, othersource's duration = %lld, mLastDequeuedTimeUs = %lld, othersource.mLastDequeuedTimeUs = %lld", 
			  idx, packetSource->getEstimatedDurationUs(), otherSource->getEstimatedDurationUs(), strm.mLastDequeuedTimeUs, otherstrm.mLastDequeuedTimeUs);

		if((strm.mLastDequeuedTimeUs >= otherstrm.mLastDequeuedTimeUs)
			|| (finalResult != OK)){
			//ALOGE("idx = %d, (otherSource != NULL && !otherSource->hasBufferAvailable(&finalResult))  strmtimeus = %lld, otherstrmtimeus = %lld return", idx, strm.mLastDequeuedTimeUs, otherstrm.mLastDequeuedTimeUs);
			return finalResult == OK ? -EAGAIN : finalResult;
		}
        }
	 
    }else{
          size_t streamIdx = 0;
          if (stream == STREAMTYPE_AUDIO) {
		streamIdx = kAudioIndex;
          } else if (stream == STREAMTYPE_VIDEO) {
		streamIdx = kVideoIndex;
          }
	   StreamItem& strm = mStreams[streamIdx];
        if(mReadSucessCount[idx] == 0)
		ALOGE("idx = %zd duration = %lld, mLastDequeuedTimeUs = %lld", idx, packetSource->getEstimatedDurationUs(), strm.mLastDequeuedTimeUs);
    }
    
   mReadSucessCount[idx] ++;
   //read 30,printf one.
   if(mReadSucessCount[idx] == 30) mReadSucessCount[idx] = 0;
   
#else
    if (otherSource != NULL && !otherSource->hasBufferAvailable(&finalResult)) {
        return finalResult == OK ? -EAGAIN : finalResult;
    }
#endif

#ifdef MTK_AOSP_ENHANCEMENT
    checkBufferingIfNecessary(false/*buffering flag*/);
#endif

    status_t err = packetSource->dequeueAccessUnit(accessUnit);

    size_t streamIdx;
    const char *streamStr;
    switch (stream) {
        case STREAMTYPE_AUDIO:
            streamIdx = kAudioIndex;
            streamStr = "audio";
            break;
        case STREAMTYPE_VIDEO:
            streamIdx = kVideoIndex;
            streamStr = "video";
            break;
        case STREAMTYPE_SUBTITLES:
            streamIdx = kSubtitleIndex;
            streamStr = "subs";
            break;
        default:
            TRESPASS();
    }

    StreamItem& strm = mStreams[streamIdx];
    if (err == INFO_DISCONTINUITY) {
        // adaptive streaming, discontinuities in the playlist
        int32_t type;
        CHECK((*accessUnit)->meta()->findInt32("discontinuity", &type));

        sp<AMessage> extra;
        if (!(*accessUnit)->meta()->findMessage("extra", &extra)) {
            extra.clear();
        }

        ALOGI("[%s] read discontinuity of type %d, extra = %s",
              streamStr,
              type,
              extra == NULL ? "NULL" : extra->debugString().c_str());

        int32_t swap;
        if ((*accessUnit)->meta()->findInt32("swapPacketSource", &swap) && swap) {
            int32_t switchGeneration;
            CHECK((*accessUnit)->meta()->findInt32("switchGeneration", &switchGeneration));
            {
                Mutex::Autolock lock(mSwapMutex);
                if (switchGeneration == mSwitchGeneration) {
                    swapPacketSource(stream);
                    sp<AMessage> msg = new AMessage(kWhatSwapped, id());
                    msg->setInt32("stream", stream);
                    msg->setInt32("switchGeneration", switchGeneration);
                    msg->post();
                }
            }
        } else {
            size_t seq = strm.mCurDiscontinuitySeq;
            int64_t offsetTimeUs;
            if (mDiscontinuityOffsetTimesUs.indexOfKey(seq) >= 0) {
                offsetTimeUs = mDiscontinuityOffsetTimesUs.valueFor(seq);
            } else {
                offsetTimeUs = 0;
            }

            seq += 1;
            if (mDiscontinuityAbsStartTimesUs.indexOfKey(strm.mCurDiscontinuitySeq) >= 0) {
                int64_t firstTimeUs;
                firstTimeUs = mDiscontinuityAbsStartTimesUs.valueFor(strm.mCurDiscontinuitySeq);
                offsetTimeUs += strm.mLastDequeuedTimeUs - firstTimeUs;
                offsetTimeUs += strm.mLastSampleDurationUs;
            } else {
                offsetTimeUs += strm.mLastSampleDurationUs;
            }

            mDiscontinuityOffsetTimesUs.add(seq, offsetTimeUs);
        }
    } else if (err == OK) {

        if (stream == STREAMTYPE_AUDIO || stream == STREAMTYPE_VIDEO) {
            int64_t timeUs;
            int32_t discontinuitySeq = 0;
            CHECK((*accessUnit)->meta()->findInt64("timeUs",  &timeUs));
            (*accessUnit)->meta()->findInt32("discontinuitySeq", &discontinuitySeq);
            strm.mCurDiscontinuitySeq = discontinuitySeq;

            int32_t discard = 0;
            int64_t firstTimeUs;
            if (mDiscontinuityAbsStartTimesUs.indexOfKey(strm.mCurDiscontinuitySeq) >= 0) {
                int64_t durUs; // approximate sample duration
                if (timeUs > strm.mLastDequeuedTimeUs) {
                    durUs = timeUs - strm.mLastDequeuedTimeUs;
                } else {
                    durUs = strm.mLastDequeuedTimeUs - timeUs;
                }
                strm.mLastSampleDurationUs = durUs;
                firstTimeUs = mDiscontinuityAbsStartTimesUs.valueFor(strm.mCurDiscontinuitySeq);
            } else if ((*accessUnit)->meta()->findInt32("discard", &discard) && discard) {
                firstTimeUs = timeUs;
            } else {
                mDiscontinuityAbsStartTimesUs.add(strm.mCurDiscontinuitySeq, timeUs);
                firstTimeUs = timeUs;
            }

            strm.mLastDequeuedTimeUs = timeUs;
	     //timestmp is computed in Atsparser.
#ifndef MTK_AOSP_ENHANCEMENT		
            if (timeUs >= firstTimeUs) {
                timeUs -= firstTimeUs;
            } else {
                timeUs = 0;
            }
            timeUs += mLastSeekTimeUs;
            if (mDiscontinuityOffsetTimesUs.indexOfKey(discontinuitySeq) >= 0) {
                timeUs += mDiscontinuityOffsetTimesUs.valueFor(discontinuitySeq);
            }
#endif
            ALOGV("[%s] read buffer at time %" PRId64 " us", streamStr, timeUs);
            (*accessUnit)->meta()->setInt64("timeUs",  timeUs);
            mLastDequeuedTimeUs = timeUs;
            mRealTimeBaseUs = ALooper::GetNowUs() - timeUs;
        } else if (stream == STREAMTYPE_SUBTITLES) {
            int32_t subtitleGeneration;
            if ((*accessUnit)->meta()->findInt32("subtitleGeneration", &subtitleGeneration)
                    && subtitleGeneration != mSubtitleGeneration) {
               return -EAGAIN;
            };
            (*accessUnit)->meta()->setInt32(
                    "trackIndex", mPlaylist->getSelectedIndex());
            (*accessUnit)->meta()->setInt64("baseUs", mRealTimeBaseUs);
        }
    } else {
        ALOGI("[%s] encountered error %d", streamStr, err);
    }

    return err;
}

status_t LiveSession::getStreamFormat(StreamType stream, sp<AMessage> *format) {
    // No swapPacketSource race condition; called from the same thread as dequeueAccessUnit.
    if (!(mStreamMask & stream)) {
#ifdef MTK_AOSP_ENHANCEMENT
		ALOGW("getStreamFormat,stream(0x%x) not found in mStreamMask(0x%x)",stream,mStreamMask);
	
#endif
        return UNKNOWN_ERROR;
    }

    sp<AnotherPacketSource> packetSource = mPacketSources.valueFor(stream);

    sp<MetaData> meta = packetSource->getFormat();

    if (meta == NULL) {
#ifdef MTK_AOSP_ENHANCEMENT
		ALOGD("getStreamFormat,no format for stream(0x%x)",stream);
#endif 
        return -EAGAIN;
    }

    return convertMetaDataToMessage(meta, format);
}

void LiveSession::connectAsync(
        const char *url, const KeyedVector<String8, String8> *headers) {
    sp<AMessage> msg = new AMessage(kWhatConnect, id());
    msg->setString("url", url);

    if (headers != NULL) {
        msg->setPointer(
                "headers",
                new KeyedVector<String8, String8>(*headers));
    }

    msg->post();
}

status_t LiveSession::disconnect() {
#ifdef MTK_AOSP_ENHANCEMENT
    ALOGI("func=%s ++, line=%d,force disconnect",__FUNCTION__, __LINE__);
    mForceDisconnect = true;
    mHTTPDataSource->disconnect();
#endif
    sp<AMessage> msg = new AMessage(kWhatDisconnect, id());

    sp<AMessage> response;
    status_t err = msg->postAndAwaitResponse(&response);
#ifdef MTK_AOSP_ENHANCEMENT
    if(mForceDisconnect == true) mForceDisconnect = false;
    ALOGI("func=%s --, line=%d,force disconnect",__FUNCTION__, __LINE__);
#endif    
    return err;
}

status_t LiveSession::seekTo(int64_t timeUs) {
#ifdef MTK_AOSP_ENHANCEMENT
ALOGI("func=%s ++, line=%d,force disconnect",__FUNCTION__, __LINE__);
    mForceDisconnect = true;
    mHTTPDataSource->disconnect();
    checkBufferingIfNecessary(true/*buffering flag*/);
#endif
    sp<AMessage> msg = new AMessage(kWhatSeek, id());
    msg->setInt64("timeUs", timeUs);
    ALOGD("seek to timeUs %lld", timeUs);
    sp<AMessage> response;
    status_t err = msg->postAndAwaitResponse(&response);

#ifdef MTK_AOSP_ENHANCEMENT
    ALOGI("seekTo -, mForceDisconnect=%d",mForceDisconnect);
    if(mForceDisconnect == true) mForceDisconnect = false;
    ALOGI("func=%s --, line=%d,force disconnect",__FUNCTION__, __LINE__);
#endif
    return err;
}

void LiveSession::onMessageReceived(const sp<AMessage> &msg) {
    switch (msg->what()) {
        case kWhatConnect:
        {
            onConnect(msg);
            break;
        }

        case kWhatDisconnect:
        {
            CHECK(msg->senderAwaitsResponse(&mDisconnectReplyID));

            if (mReconfigurationInProgress) {
                break;
            }

            finishDisconnect();
            break;
        }

        case kWhatSeek:
        {
            uint32_t seekReplyID;
            CHECK(msg->senderAwaitsResponse(&seekReplyID));
            mSeekReplyID = seekReplyID;
            mSeekReply = new AMessage;

            status_t err = onSeek(msg);

            if (err != OK) {
                msg->post(50000);
            }
            break;
        }

        case kWhatFetcherNotify:
        {
            int32_t what;
            CHECK(msg->findInt32("what", &what));

            switch (what) {
                case PlaylistFetcher::kWhatStarted:
                    break;
                case PlaylistFetcher::kWhatPaused:
                case PlaylistFetcher::kWhatStopped:
                {
                    if (what == PlaylistFetcher::kWhatStopped) {
#ifdef MTK_AOSP_ENHANCEMENT
                        Mutex::Autolock autoLock(mLock);
#endif                        
                        AString uri;
                        CHECK(msg->findString("uri", &uri));
                        ALOGD("delete PLF %s", uri.c_str());
                        if (mFetcherInfos.removeItem(uri) < 0) {
                            // ignore duplicated kWhatStopped messages.
                            ALOGW("repeat delete the same PLF");
                            break;
                        }

                        if (mSwitchInProgress) {
                            ALOGW("why we are here, we call cancelBWSwitch, which will set false");
                            tryToFinishBandwidthSwitch();
                        }
                    }
                    ALOGD("reveive kWhatPaused or kWhatStopped, mContinuationCounter = %zu", mContinuationCounter);
                    if (mContinuation != NULL) {
                        CHECK_GT(mContinuationCounter, 0);
                        if (--mContinuationCounter == 0) {
                            mContinuation->post();

                            if (mSeekReplyID != 0) {
                                CHECK(mSeekReply != NULL);
                                mSeekReply->setInt32("err", OK);
                                mSeekReply->postReply(mSeekReplyID);
                                mSeekReplyID = 0;
                                mSeekReply.clear();
                            }
                        }
                    }
                    break;
                }

                case PlaylistFetcher::kWhatDurationUpdate:
                {
                    AString uri;
                    CHECK(msg->findString("uri", &uri));

                    int64_t durationUs;
                    CHECK(msg->findInt64("durationUs", &durationUs));
#ifdef MTK_AOSP_ENHANCEMENT
                   //maybe on changing, fetcherinfo delete.
                   if(mFetcherInfos.indexOfKey(uri) < 0){
                        ALOGD("kWhatDurationUpdate : mFetcherInfos already delete");
                        break;
                   }
#endif
                    FetcherInfo *info = &mFetcherInfos.editValueFor(uri);
                    info->mDurationUs = durationUs;
                    break;
                }

                case PlaylistFetcher::kWhatError:
                {
                    status_t err;
                    CHECK(msg->findInt32("err", &err));

                    ALOGE("XXX Received error %d from PlaylistFetcher.", err);

#ifdef MTK_AOSP_ENHANCEMENT
                    if(mForceDisconnect && (err == ERROR_IO)) {
                        ALOGD("ignore the socket IO error when force disconnect");
                        break;
                    }else if(mForceDisconnect ){
						ALOGD("we miss a error %d when force disconnect", err);
                               }
#endif

                    // handle EOS on subtitle tracks independently
                    AString uri;
                    if (err == ERROR_END_OF_STREAM && msg->findString("uri", &uri)) {
                        ssize_t i = mFetcherInfos.indexOfKey(uri);
                        if (i >= 0) {
                            const sp<PlaylistFetcher> &fetcher = mFetcherInfos.valueAt(i).mFetcher;
                            if (fetcher != NULL) {
                                uint32_t type = fetcher->getStreamTypeMask();
                                if (type == STREAMTYPE_SUBTITLES) {
                                    mPacketSources.valueFor(
                                            STREAMTYPE_SUBTITLES)->signalEOS(err);;
                                    break;
                                }
                            }
                        }
                    }

                    if (mInPreparationPhase) {
                        postPrepared(err);
                    }
#ifdef MTK_AOSP_ENHANCEMENT
                    if(err == ERROR_END_OF_STREAM){
                        checkBufferingIfNecessary(false); 
                        ssize_t i = mFetcherInfos.indexOfKey(uri);
                        if(i >= 0){
                        FetcherInfo *info = &mFetcherInfos.editValueFor(uri);
                        info->mIsPrepared = true;
                        }
                    }
#endif
                    cancelBandwidthSwitch();
                    mPacketSources.valueFor(STREAMTYPE_AUDIO)->signalEOS(err);

                    mPacketSources.valueFor(STREAMTYPE_VIDEO)->signalEOS(err);

                    mPacketSources.valueFor(
                            STREAMTYPE_SUBTITLES)->signalEOS(err);

                    sp<AMessage> notify = mNotify->dup();
                    notify->setInt32("what", kWhatError);
                    notify->setInt32("err", err);
                    notify->post();
                    break;
                }

                case PlaylistFetcher::kWhatTemporarilyDoneFetching:
                {
                    AString uri;
                    CHECK(msg->findString("uri", &uri));

                    if (mFetcherInfos.indexOfKey(uri) < 0) {
                        ALOGE("couldn't find uri");
                        break;
                    }
                    FetcherInfo *info = &mFetcherInfos.editValueFor(uri);
                    info->mIsPrepared = true;

                    if (mInPreparationPhase) {
                        bool allFetchersPrepared = true;
                        for (size_t i = 0; i < mFetcherInfos.size(); ++i) {
                            if (!mFetcherInfos.valueAt(i).mIsPrepared) {
                                allFetchersPrepared = false;
                                break;
                            }
                        }

                        if (allFetchersPrepared) {
                            postPrepared(OK);
                        }
                    }
                    break;
                }

                case PlaylistFetcher::kWhatStartedAt:
                {
                    int32_t switchGeneration;
                    CHECK(msg->findInt32("switchGeneration", &switchGeneration));

                    if (switchGeneration != mSwitchGeneration) {
                        break;
                    }

                    // Resume fetcher for the original variant; the resumed fetcher should
                    // continue until the timestamps found in msg, which is stored by the
                    // new fetcher to indicate where the new variant has started buffering.
                    for (size_t i = 0; i < mFetcherInfos.size(); i++) {
                        const FetcherInfo info = mFetcherInfos.valueAt(i);
                        if (info.mToBeRemoved) {
                            info.mFetcher->resumeUntilAsync(msg);
                        }
                    }
                    break;
                }
#ifdef MTK_AOSP_ENHANCEMENT
                case PlaylistFetcher::kWhatPicture: 
                {
                    onPictureReceived(msg);
                    break;
                }
#endif
                default:
                    TRESPASS();
            }

            break;
        }

        case kWhatCheckBandwidth:
        {
            int32_t generation;
            CHECK(msg->findInt32("generation", &generation));

            if (generation != mCheckBandwidthGeneration) {
                break;
            }

            onCheckBandwidth(msg);
            break;
        }

        case kWhatChangeConfiguration:
        {
            onChangeConfiguration(msg);
            break;
        }

        case kWhatChangeConfiguration2:
        {
            onChangeConfiguration2(msg);
            break;
        }

        case kWhatChangeConfiguration3:
        {
            onChangeConfiguration3(msg);
            break;
        }

        case kWhatFinishDisconnect2:
        {
            onFinishDisconnect2();
            break;
        }

        case kWhatSwapped:
        {
            onSwapped(msg);
            break;
        }

        case kWhatCheckSwitchDown:
        {
            onCheckSwitchDown();
            break;
        }

        case kWhatSwitchDown:
        {
            onSwitchDown();
            break;
        }

        default:
            TRESPASS();
            break;
    }
}

// static
int LiveSession::SortByBandwidth(const BandwidthItem *a, const BandwidthItem *b) {
    if (a->mBandwidth < b->mBandwidth) {
        return -1;
    } else if (a->mBandwidth == b->mBandwidth) {
        return 0;
    }

    return 1;
}

// static
LiveSession::StreamType LiveSession::indexToType(int idx) {
    CHECK(idx >= 0 && idx < kMaxStreams);
    return (StreamType)(1 << idx);
}

// static
ssize_t LiveSession::typeToIndex(int32_t type) {
    switch (type) {
        case STREAMTYPE_AUDIO:
            return 0;
        case STREAMTYPE_VIDEO:
            return 1;
        case STREAMTYPE_SUBTITLES:
            return 2;
        default:
            return -1;
    };
    return -1;
}

void LiveSession::onConnect(const sp<AMessage> &msg) {
    AString url;
    CHECK(msg->findString("url", &url));

    KeyedVector<String8, String8> *headers = NULL;
    if (!msg->findPointer("headers", (void **)&headers)) {
        mExtraHeaders.clear();
    } else {
        mExtraHeaders = *headers;

        delete headers;
        headers = NULL;
    }

    // TODO currently we don't know if we are coming here from incognito mode
    ALOGI("onConnect %s", uriDebugString(url).c_str());

    mMasterURL = url;

    bool dummy;
#ifdef MTK_AOSP_ENHANCEMENT
    status_t errcode;
    mPlaylist = fetchPlaylist(url.c_str(), NULL /* curPlaylistHash */, &dummy, &errcode);
#else
    mPlaylist = fetchPlaylist(url.c_str(), NULL /* curPlaylistHash */, &dummy);
#endif

    if (mPlaylist == NULL) {
        ALOGE("unable to fetch master playlist %s.", uriDebugString(url).c_str());

        postPrepared(ERROR_IO);
        return;
    }

    // We trust the content provider to make a reasonable choice of preferred
    // initial bandwidth by listing it first in the variant playlist.
    // At startup we really don't have a good estimate on the available
    // network bandwidth since we haven't tranferred any data yet. Once
    // we have we can make a better informed choice.
    size_t initialBandwidth = 0;
    size_t initialBandwidthIndex = 0;

    if (mPlaylist->isVariantPlaylist()) {
        for (size_t i = 0; i < mPlaylist->size(); ++i) {
            BandwidthItem item;

            item.mPlaylistIndex = i;

            sp<AMessage> meta;
            AString uri;
            mPlaylist->itemAt(i, &uri, &meta);

            unsigned long bandwidth;
            CHECK(meta->findInt32("bandwidth", (int32_t *)&item.mBandwidth));

            if (initialBandwidth == 0) {
                initialBandwidth = item.mBandwidth;
            }

            mBandwidthItems.push(item);
        }

        CHECK_GT(mBandwidthItems.size(), 0u);

        mBandwidthItems.sort(SortByBandwidth);

        for (size_t i = 0; i < mBandwidthItems.size(); ++i) {
            if (mBandwidthItems.itemAt(i).mBandwidth == initialBandwidth) {
                initialBandwidthIndex = i;
                break;
            }
        }
    } else {
        // dummy item.
        BandwidthItem item;
        item.mPlaylistIndex = 0;
        item.mBandwidth = 0;
        mBandwidthItems.push(item);
    }

    mPlaylist->pickRandomMediaItems();
    changeConfiguration(
            0ll /* timeUs */, initialBandwidthIndex, false /* pickTrack */);
}

void LiveSession::finishDisconnect() {
    // No reconfiguration is currently pending, make sure none will trigger
    // during disconnection either.
    cancelCheckBandwidthEvent();

    // Protect mPacketSources from a swapPacketSource race condition through disconnect.
    // (finishDisconnect, onFinishDisconnect2)
    cancelBandwidthSwitch();

    // cancel switch down monitor
    mSwitchDownMonitor.clear();

    for (size_t i = 0; i < mFetcherInfos.size(); ++i) {
        mFetcherInfos.valueAt(i).mFetcher->stopAsync();
    }

    sp<AMessage> msg = new AMessage(kWhatFinishDisconnect2, id());

    mContinuationCounter = mFetcherInfos.size();
    mContinuation = msg;

    if (mContinuationCounter == 0) {
        msg->post();
    }
}

void LiveSession::onFinishDisconnect2() {
    mContinuation.clear();

    mPacketSources.valueFor(STREAMTYPE_AUDIO)->signalEOS(ERROR_END_OF_STREAM);
    mPacketSources.valueFor(STREAMTYPE_VIDEO)->signalEOS(ERROR_END_OF_STREAM);

    mPacketSources.valueFor(
            STREAMTYPE_SUBTITLES)->signalEOS(ERROR_END_OF_STREAM);

    sp<AMessage> response = new AMessage;
    response->setInt32("err", OK);

    response->postReply(mDisconnectReplyID);
    mDisconnectReplyID = 0;
}

sp<PlaylistFetcher> LiveSession::addFetcher(const char *uri) {
#ifdef MTK_AOSP_ENHANCEMENT
		ALOGI("addFetcher,uri = %s",uri);
#endif
    ssize_t index = mFetcherInfos.indexOfKey(uri);

    if (index >= 0) {
        return NULL;
    }

    sp<AMessage> notify = new AMessage(kWhatFetcherNotify, id());
    notify->setString("uri", uri);
    notify->setInt32("switchGeneration", mSwitchGeneration);
#ifdef MTK_AOSP_ENHANCEMENT
    Mutex::Autolock autoLock(mLock);
#endif
    FetcherInfo info;
    info.mFetcher = new PlaylistFetcher(notify, this, uri, mSubtitleGeneration);
    info.mDurationUs = -1ll;
    info.mIsPrepared = false;
    info.mToBeRemoved = false;
#ifdef MTK_AOSP_ENHANCEMENT
    //create other looper inner.
    mPlaylistfetcherLooper->registerHandler(info.mFetcher);
#else
    looper()->registerHandler(info.mFetcher);
#endif
    mFetcherInfos.add(uri, info);

    return info.mFetcher;
}

/*
 * Illustration of parameters:
 *
 * 0      `range_offset`
 * +------------+-------------------------------------------------------+--+--+
 * |            |                                 | next block to fetch |  |  |
 * |            | `source` handle => `out` buffer |                     |  |  |
 * | `url` file |<--------- buffer size --------->|<--- `block_size` -->|  |  |
 * |            |<----------- `range_length` / buffer capacity ----------->|  |
 * |<------------------------------ file_size ------------------------------->|
 *
 * Special parameter values:
 * - range_length == -1 means entire file
 * - block_size == 0 means entire range
 *
 */
ssize_t LiveSession::fetchFile(
        const char *url, sp<ABuffer> *out,
        int64_t range_offset, int64_t range_length,
        uint32_t block_size, /* download block size */
        sp<DataSource> *source, /* to return and reuse source */
        String8 *actualUrl) {
    off64_t size;
    sp<DataSource> temp_source;
    if (source == NULL) {
        source = &temp_source;
    }

    if (*source == NULL) {
        if (!strncasecmp(url, "file://", 7)) {
            *source = new FileSource(url + 7);
        } else if (strncasecmp(url, "http://", 7)
                && strncasecmp(url, "https://", 8)) {
            return ERROR_UNSUPPORTED;
        } else {
            KeyedVector<String8, String8> headers = mExtraHeaders;
            if (range_offset > 0 || range_length >= 0) {
                headers.add(
                        String8("Range"),
                        String8(
                            StringPrintf(
                                "bytes=%lld-%s",
                                range_offset,
                                range_length < 0
                                    ? "" : StringPrintf("%lld",
                                            range_offset + range_length - 1).c_str()).c_str()));
            }
            int64_t nowUs = ALooper::GetNowUs();
            ALOGD("connect ++ this http url= %s", url);
            status_t err = mHTTPDataSource->connect(url, &headers);
            ALOGD("connect -- %lld us", ALooper::GetNowUs()-nowUs);

            if (err != OK) {
#ifdef MTK_AOSP_ENHANCEMENT
				ALOGW("HTTP data source connect return err =0x%x",err);
                return ERROR_IO;
#endif 
                return err;
            }

            *source = mHTTPDataSource;
        }
    }

    status_t getSizeErr = (*source)->getSize(&size);
    if (getSizeErr != OK) {
        ALOGE("getSize error %d", getSizeErr);
        size = 65536;
    }

    sp<ABuffer> buffer = *out != NULL ? *out : new ABuffer(size);
    if (*out == NULL) {
        buffer->setRange(0, 0);
    }

    ssize_t bytesRead = 0;
    // adjust range_length if only reading partial block
    if (block_size > 0 && (range_length == -1 || (int64_t)(buffer->size() + block_size) < range_length)) {
        range_length = buffer->size() + block_size;
    }
    for (;;) {
        // Only resize when we don't know the size.
        size_t bufferRemaining = buffer->capacity() - buffer->size();
        if (bufferRemaining == 0 && getSizeErr != OK) {
            size_t bufferIncrement = buffer->size() / 2;
            if (bufferIncrement < 32768) {
                bufferIncrement = 32768;
            }
            bufferRemaining = bufferIncrement;

            ALOGV("increasing download buffer to %zu bytes",
                 buffer->size() + bufferRemaining);

            sp<ABuffer> copy = new ABuffer(buffer->size() + bufferRemaining);
            memcpy(copy->data(), buffer->data(), buffer->size());
            copy->setRange(0, buffer->size());

            buffer = copy;
        }

        size_t maxBytesToRead = bufferRemaining;
        if (range_length >= 0) {
            int64_t bytesLeftInRange = range_length - buffer->size();
            if (bytesLeftInRange < (int64_t)maxBytesToRead) {
                maxBytesToRead = bytesLeftInRange;

                if (bytesLeftInRange == 0) {
                    break;
                }
            }
        }

        // The DataSource is responsible for informing us of error (n < 0) or eof (n == 0)
        // to help us break out of the loop.
        ssize_t n = (*source)->readAt(
                buffer->size(), buffer->data() + buffer->size(),
                maxBytesToRead);

        if (n < 0) {
	     ALOGD("n < 0 n =%zd offset =%zu, maxBytesToRead = %d return", n, buffer->size(), maxBytesToRead);
#ifdef MTK_AOSP_ENHANCEMENT
            return ERROR_IO;
#endif
            return n;
        }

        if (n == 0) {
            break;
        }

        buffer->setRange(0, buffer->size() + (size_t)n);
        bytesRead += n;
    }

    *out = buffer;
    if (actualUrl != NULL) {
        *actualUrl = (*source)->getUri();
        if (actualUrl->isEmpty()) {
            *actualUrl = url;
        }
    }

    return bytesRead;
}
#ifdef MTK_AOSP_ENHANCEMENT
sp<M3UParser> LiveSession::fetchPlaylist(
        const char *url, uint8_t *curPlaylistHash, bool *unchanged, status_t *pErrCode) {
    *pErrCode = OK;
    ALOGI("fetchPlaylist '%s'", url);
#else
sp<M3UParser> LiveSession::fetchPlaylist(
        const char *url, uint8_t *curPlaylistHash, bool *unchanged) {
#endif
    ALOGV("fetchPlaylist '%s'", url);

    *unchanged = false;

    sp<ABuffer> buffer;
    String8 actualUrl;
    ssize_t  err = fetchFile(url, &buffer, 0, -1, 0, NULL, &actualUrl);

    if (err <= 0) {
#ifdef MTK_AOSP_ENHANCEMENT    
        //if(mForceDisconnect == true) {
            *pErrCode = err;
            ALOGD("Error code:%d",err);
        //}
#endif
        return NULL;
    }
#ifdef MTK_AOSP_ENHANCEMENT    
    dumpPlaylist(buffer);
#endif

    // MD5 functionality is not available on the simulator, treat all
    // playlists as changed.

#if defined(HAVE_ANDROID_OS)
    uint8_t hash[16];

    MD5_CTX m;
    MD5_Init(&m);
    MD5_Update(&m, buffer->data(), buffer->size());

    MD5_Final(hash, &m);

    if (curPlaylistHash != NULL && !memcmp(hash, curPlaylistHash, 16)) {
        // playlist unchanged
        *unchanged = true;

        return NULL;
    }

    if (curPlaylistHash != NULL) {
        memcpy(curPlaylistHash, hash, sizeof(hash));
    }
#endif

    sp<M3UParser> playlist =
        new M3UParser(actualUrl.string(), buffer->data(), buffer->size());

    if (playlist->initCheck() != OK) {
        ALOGE("failed to parse .m3u8 playlist");

        return NULL;
    }

    return playlist;
}

static double uniformRand() {
    return (double)rand() / RAND_MAX;
}

size_t LiveSession::getBandwidthIndex() {
    if (mBandwidthItems.size() == 0) {
        return 0;
    }

#if 1
    char value[PROPERTY_VALUE_MAX];
    ssize_t index = -1;
    if (property_get("media.httplive.bw-index", value, NULL)) {
        char *end;
        index = strtol(value, &end, 10);
        CHECK(end > value && *end == '\0');

        if (index >= 0 && (size_t)index >= mBandwidthItems.size()) {
            index = mBandwidthItems.size() - 1;
        }
    }

    if (index < 0) {
#ifdef MTK_AOSP_ENHANCEMENT
        int32_t bandwidthBps;
        int32_t count_depth = 0; // playlistfetcher 100kb one download.
        ssize_t lastindex;
        if(mHTTPDataSource == NULL){
            ALOGD("mHTTPDataSource is null return bandwidthindex = 0");
            return 0;
        }
        index = mCurBandwidthIndex;
        lastindex = mCurBandwidthIndex; 
       do{
              lastindex =  index;
              //count_depth: 10s data need download how many fetchfile
              count_depth = mBandwidthItems.itemAt(lastindex).mBandwidth * 10 / 12800 /8;
              ALOGD("count_depth = %d, lastindex = %d, bandwith = %lu", count_depth, lastindex, mBandwidthItems.itemAt(lastindex).mBandwidth);
              if ( mHTTPDataSource->estimateBandwidth(count_depth, &bandwidthBps)) {
                  index = pickBandwidthIndex(bandwidthBps);
                  ALOGI("bandwidth estimated at countdepth %d , [bandwith]%.2f kbps, index = %d", count_depth, bandwidthBps / 1024.0f, index);
              } else {
                  ALOGV("no bandwidth estimate.");
              }
        }while(index > lastindex);
                
	 ALOGD("getBandwidthIndex = %zd", index);
#else
        int32_t bandwidthBps;
        if (mHTTPDataSource != NULL
                && mHTTPDataSource->estimateBandwidth(&bandwidthBps)) {
            ALOGV("bandwidth estimated at %.2f kbps", bandwidthBps / 1024.0f);
        } else {
            ALOGV("no bandwidth estimate.");
            return 0;  // Pick the lowest bandwidth stream by default.
        }

        char value[PROPERTY_VALUE_MAX];
        if (property_get("media.httplive.max-bw", value, NULL)) {
            char *end;
            long maxBw = strtoul(value, &end, 10);
            if (end > value && *end == '\0') {
                if (maxBw > 0 && bandwidthBps > maxBw) {
                    ALOGV("bandwidth capped to %ld bps", maxBw);
                    bandwidthBps = maxBw;
                }
            }
        }
        // Pick the highest bandwidth stream below or equal to estimated bandwidth.

        index = mBandwidthItems.size() - 1;
        while (index > 0) {
            // consider only 80% of the available bandwidth, but if we are switching up,
            // be even more conservative (70%) to avoid overestimating and immediately
            // switching back.
            size_t adjustedBandwidthBps = bandwidthBps;
            if (index > mCurBandwidthIndex) {
                adjustedBandwidthBps = adjustedBandwidthBps * 7 / 10;
            } else {
                adjustedBandwidthBps = adjustedBandwidthBps * 8 / 10;
            }
            if (mBandwidthItems.itemAt(index).mBandwidth <= adjustedBandwidthBps) {
                break;
            }
            --index;
        }
#endif
    }
#elif 0
    // Change bandwidth at random()
    size_t index = uniformRand() * mBandwidthItems.size();
#elif 0
    // There's a 50% chance to stay on the current bandwidth and
    // a 50% chance to switch to the next higher bandwidth (wrapping around
    // to lowest)
    const size_t kMinIndex = 0;

    static ssize_t mCurBandwidthIndex = -1;

    size_t index;
    if (mCurBandwidthIndex < 0) {
        index = kMinIndex;
    } else if (uniformRand() < 0.5) {
        index = (size_t)mCurBandwidthIndex;
    } else {
        index = mCurBandwidthIndex + 1;
        if (index == mBandwidthItems.size()) {
            index = kMinIndex;
        }
    }
    mCurBandwidthIndex = index;
#elif 0
    // Pick the highest bandwidth stream below or equal to 1.2 Mbit/sec

    size_t index = mBandwidthItems.size() - 1;
    while (index > 0 && mBandwidthItems.itemAt(index).mBandwidth > 1200000) {
        --index;
    }
#elif 1
    char value[PROPERTY_VALUE_MAX];
    size_t index;
    if (property_get("media.httplive.bw-index", value, NULL)) {
        char *end;
        index = strtoul(value, &end, 10);
        CHECK(end > value && *end == '\0');

        if (index >= mBandwidthItems.size()) {
            index = mBandwidthItems.size() - 1;
        }
    } else {
        index = 0;
    }
#else
    size_t index = mBandwidthItems.size() - 1;  // Highest bandwidth stream
#endif

    CHECK_GE(index, 0);

    return index;
}

int64_t LiveSession::latestMediaSegmentStartTimeUs() {
    sp<AMessage> audioMeta = mPacketSources.valueFor(STREAMTYPE_AUDIO)->getLatestDequeuedMeta();
    int64_t minSegmentStartTimeUs = -1, videoSegmentStartTimeUs = -1;
    if (audioMeta != NULL) {
        audioMeta->findInt64("segmentStartTimeUs", &minSegmentStartTimeUs);
    }

    sp<AMessage> videoMeta = mPacketSources.valueFor(STREAMTYPE_VIDEO)->getLatestDequeuedMeta();
    if (videoMeta != NULL
            && videoMeta->findInt64("segmentStartTimeUs", &videoSegmentStartTimeUs)) {
        if (minSegmentStartTimeUs < 0 || videoSegmentStartTimeUs < minSegmentStartTimeUs) {
            minSegmentStartTimeUs = videoSegmentStartTimeUs;
        }

    }
    return minSegmentStartTimeUs;
}

status_t LiveSession::onSeek(const sp<AMessage> &msg) {
    int64_t timeUs;
    CHECK(msg->findInt64("timeUs", &timeUs));

    if (!mReconfigurationInProgress) {
#ifdef MTK_AOSP_ENHANCEMENT
        mLastDequeuedTimeUs = -1;
#endif
        changeConfiguration(timeUs, mCurBandwidthIndex);
        return OK;
    } else {
        return -EWOULDBLOCK;
    }
}

status_t LiveSession::getDuration(int64_t *durationUs) const {
    int64_t maxDurationUs = -1ll;
    for (size_t i = 0; i < mFetcherInfos.size(); ++i) {
        int64_t fetcherDurationUs = mFetcherInfos.valueAt(i).mDurationUs;

        if (fetcherDurationUs > maxDurationUs) {
            maxDurationUs = fetcherDurationUs;
        }
    }

    *durationUs = maxDurationUs;
#ifdef MTK_AOSP_ENHANCEMENT
	ALOGI("getDuration,durationUs=%" PRId64 " us",*durationUs);
#endif 
    return OK;
}

bool LiveSession::isSeekable() const {
    int64_t durationUs;
#ifdef MTK_AOSP_ENHANCEMENT
    return getDuration(&durationUs) == OK && durationUs > 0;
#else
    return getDuration(&durationUs) == OK && durationUs >= 0;
#endif    
}

bool LiveSession::hasDynamicDuration() const {
    return false;
}

size_t LiveSession::getTrackCount() const {
    if (mPlaylist == NULL) {
        return 0;
    } else {
        return mPlaylist->getTrackCount();
    }
}

sp<AMessage> LiveSession::getTrackInfo(size_t trackIndex) const {
    if (mPlaylist == NULL) {
        return NULL;
    } else {
        return mPlaylist->getTrackInfo(trackIndex);
    }
}

status_t LiveSession::selectTrack(size_t index, bool select) {
#ifdef MTK_AOSP_ENHANCEMENT
		ALOGI("selectTrack,index=%d,select=%d",index,select);
#endif
    if (mPlaylist == NULL) {
        return INVALID_OPERATION;
    }

    ++mSubtitleGeneration;
    status_t err = mPlaylist->selectTrack(index, select);
    if (err == OK) {
        sp<AMessage> msg = new AMessage(kWhatChangeConfiguration, id());
        msg->setInt32("bandwidthIndex", mCurBandwidthIndex);
        msg->setInt32("pickTrack", select);
        msg->post();
    }
    return err;
}

ssize_t LiveSession::getSelectedTrack(media_track_type type) const {
    if (mPlaylist == NULL) {
        return -1;
    } else {
        return mPlaylist->getSelectedTrack(type);
    }
}

#ifdef MTK_AOSP_ENHANCEMENT
bool LiveSession::canSwitchUp(bool upwardsFlag) {
#else
bool LiveSession::canSwitchUp() {
#endif
    // Allow upwards bandwidth switch when a stream has buffered at least 10 seconds.
    status_t err = OK;
    for (size_t i = 0; i < mPacketSources.size(); ++i) {
        sp<AnotherPacketSource> source = mPacketSources.valueAt(i);
        int64_t dur = source->getBufferedDurationUs(&err);
#ifdef MTK_AOSP_ENHANCEMENT
		ALOGI("canSwitchUp,track %d,dur = %" PRId64 " us",i,dur);
        if(upwardsFlag) {//need refine:whether all track need >10s,or one track need >10
        				 //same track state may result in different result,if in difference order
            if (err == OK && dur > 10000000) {
                return true;
            }
            return false;
        } else { //need refine:whether all track need >15s,or one track need >15
            if (err == OK && dur > 10000000) {//15000000
                return false;
            }
            return true;
        }
#else
        if (err == OK && dur > 10000000) {
            return true;
        }
#endif
    }
    return false;
}

void LiveSession::changeConfiguration(
        int64_t timeUs, size_t bandwidthIndex, bool pickTrack) {
    // Protect mPacketSources from a swapPacketSource race condition through reconfiguration.
    // (changeConfiguration, onChangeConfiguration2, onChangeConfiguration3).
    cancelBandwidthSwitch();

    CHECK(!mReconfigurationInProgress);
    mReconfigurationInProgress = true;

#ifdef MTK_AOSP_ENHANCEMENT
	ALOGD("changeConfiguration => timeUs:%" PRId64 " us, mCurBandwidthIndex:%zu->new bwIndex:%zu, pickTrack:%d",
			  timeUs, mCurBandwidthIndex,bandwidthIndex, pickTrack);
#endif
    mCurBandwidthIndex = bandwidthIndex;

    ALOGV("changeConfiguration => timeUs:%" PRId64 " us, bwIndex:%zu, pickTrack:%d",
          timeUs, bandwidthIndex, pickTrack);

    CHECK_LT(bandwidthIndex, mBandwidthItems.size());
    const BandwidthItem &item = mBandwidthItems.itemAt(bandwidthIndex);

    uint32_t streamMask = 0; // streams that should be fetched by the new fetcher
    uint32_t resumeMask = 0; // streams that should be fetched by the original fetcher

    AString URIs[kMaxStreams];
    for (size_t i = 0; i < kMaxStreams; ++i) {
        if (mPlaylist->getTypeURI(item.mPlaylistIndex, mStreams[i].mType, &URIs[i])) {
            streamMask |= indexToType(i);
#ifdef MTK_AOSP_ENHANCEMENT
			ALOGI("changeConfiguration,new playlist StreamMask -> 0x%x,%s track uri: %s",\
				streamMask,mStreams[i].mType,URIs[i].c_str());
#endif			
        }
    }

    // Step 1, stop and discard fetchers that are no longer needed.
    // Pause those that we'll reuse.
    for (size_t i = 0; i < mFetcherInfos.size(); ++i) {
        const AString &uri = mFetcherInfos.keyAt(i);

        bool discardFetcher = true;

        // If we're seeking all current fetchers are discarded.
        if (timeUs < 0ll) {
            // delay fetcher removal if not picking tracks
            discardFetcher = pickTrack;

            for (size_t j = 0; j < kMaxStreams; ++j) {
                StreamType type = indexToType(j);
                if ((streamMask & type) && uri == URIs[j]) {
                    resumeMask |= type;
                    streamMask &= ~type;
                    discardFetcher = false;
                }
            }
        }

        if (discardFetcher) {
			ALOGI("changeConfiguration,stop fetcher %i,%s",i,uri.c_str());
            mFetcherInfos.valueAt(i).mFetcher->stopAsync();
        } else {
        	ALOGI("changeConfiguration,pause fetcher %i,%s",i,uri.c_str());
            mFetcherInfos.valueAt(i).mFetcher->pauseAsync();
        }
    }

    sp<AMessage> msg;
    if (timeUs < 0ll) {
        // skip onChangeConfiguration2 (decoder destruction) if not seeking.
        msg = new AMessage(kWhatChangeConfiguration3, id());
    } else {
        msg = new AMessage(kWhatChangeConfiguration2, id());
    }
    msg->setInt32("streamMask", streamMask);
    msg->setInt32("resumeMask", resumeMask);
    msg->setInt32("pickTrack", pickTrack);
    msg->setInt64("timeUs", timeUs);
    for (size_t i = 0; i < kMaxStreams; ++i) {
        if ((streamMask | resumeMask) & indexToType(i)) {
            msg->setString(mStreams[i].uriKey().c_str(), URIs[i].c_str());
        }
    }

    // Every time a fetcher acknowledges the stopAsync or pauseAsync request
    // we'll decrement mContinuationCounter, once it reaches zero, i.e. all
    // fetchers have completed their asynchronous operation, we'll post
    // mContinuation, which then is handled below in onChangeConfiguration2.
    mContinuationCounter = mFetcherInfos.size();
    mContinuation = msg;

    if (mContinuationCounter == 0) {
        msg->post();

        if (mSeekReplyID != 0) {
            CHECK(mSeekReply != NULL);
            mSeekReply->setInt32("err", OK);
            mSeekReply->postReply(mSeekReplyID);
            mSeekReplyID = 0;
            mSeekReply.clear();
        }
    }
}

void LiveSession::onChangeConfiguration(const sp<AMessage> &msg) {
    if (!mReconfigurationInProgress) {
        int32_t pickTrack = 0, bandwidthIndex = mCurBandwidthIndex;
        msg->findInt32("pickTrack", &pickTrack);
        msg->findInt32("bandwidthIndex", &bandwidthIndex);
        changeConfiguration(-1ll /* timeUs */, bandwidthIndex, pickTrack);
    } else {
        msg->post(1000000ll); // retry in 1 sec
    }
}

void LiveSession::onChangeConfiguration2(const sp<AMessage> &msg) {
    mContinuation.clear();

    // All fetchers are either suspended or have been removed now.

    uint32_t streamMask, resumeMask;
    CHECK(msg->findInt32("streamMask", (int32_t *)&streamMask));
    CHECK(msg->findInt32("resumeMask", (int32_t *)&resumeMask));

    // currently onChangeConfiguration2 is only called for seeking;
    // remove the following CHECK if using it else where.
    CHECK_EQ(resumeMask, 0);
    streamMask |= resumeMask;

    AString URIs[kMaxStreams];
    for (size_t i = 0; i < kMaxStreams; ++i) {
        if (streamMask & indexToType(i)) {
            const AString &uriKey = mStreams[i].uriKey();
            CHECK(msg->findString(uriKey.c_str(), &URIs[i]));
#ifdef MTK_AOSP_ENHANCEMENT
			ALOGD("onChangeConfiguration2,%s = '%s'", uriKey.c_str(), URIs[i].c_str());
#endif
            ALOGV("%s = '%s'", uriKey.c_str(), URIs[i].c_str());
        }
    }

    // Determine which decoders to shutdown on the player side,
    // a decoder has to be shutdown if either
    // 1) its streamtype was active before but now longer isn't.
    // or
    // 2) its streamtype was already active and still is but the URI
    //    has changed.
    uint32_t changedMask = 0;
    for (size_t i = 0; i < kMaxStreams && i != kSubtitleIndex; ++i) {
        if (((mStreamMask & streamMask & indexToType(i))
                && !(URIs[i] == mStreams[i].mUri))
                || (mStreamMask & ~streamMask & indexToType(i))) {
            changedMask |= indexToType(i);
        }
    }
#ifdef MTK_AOSP_ENHANCEMENT
	ALOGI("onChangeConfiguration2,changedMask = 0x%x",changedMask);
#endif

    if (changedMask == 0) {
        // If nothing changed as far as the audio/video decoders
        // are concerned we can proceed.
        onChangeConfiguration3(msg);
        return;
    }
#ifdef MTK_AOSP_ENHANCEMENT
    if (mDisconnectReplyID != 0) {
        finishDisconnect();
        return;
    }
#endif
    // Something changed, inform the player which will shutdown the
    // corresponding decoders and will post the reply once that's done.
    // Handling the reply will continue executing below in
    // onChangeConfiguration3.
    sp<AMessage> notify = mNotify->dup();
    notify->setInt32("what", kWhatStreamsChanged);
    notify->setInt32("changedMask", changedMask);

    msg->setWhat(kWhatChangeConfiguration3);
    msg->setTarget(id());

    notify->setMessage("reply", msg);
    notify->post();
}

void LiveSession::onChangeConfiguration3(const sp<AMessage> &msg) {
    mContinuation.clear();
    // All remaining fetchers are still suspended, the player has shutdown
    // any decoders that needed it.

    uint32_t streamMask, resumeMask;
    CHECK(msg->findInt32("streamMask", (int32_t *)&streamMask));
    CHECK(msg->findInt32("resumeMask", (int32_t *)&resumeMask));

    int64_t timeUs;
    int32_t pickTrack;
    bool switching = false;
    CHECK(msg->findInt64("timeUs", &timeUs));
    CHECK(msg->findInt32("pickTrack", &pickTrack));

    if (timeUs < 0ll) {
        if (!pickTrack) {
            switching = true;
        }
        mRealTimeBaseUs = ALooper::GetNowUs() - mLastDequeuedTimeUs;
    } else {
        mRealTimeBaseUs = ALooper::GetNowUs() - timeUs;
    }

    for (size_t i = 0; i < kMaxStreams; ++i) {
        if (streamMask & indexToType(i)) {
            if (switching) {
                CHECK(msg->findString(mStreams[i].uriKey().c_str(), &mStreams[i].mNewUri));
            } else {
                CHECK(msg->findString(mStreams[i].uriKey().c_str(), &mStreams[i].mUri));
            }
        }
    }

    mNewStreamMask = streamMask | resumeMask;
    if (switching) {
        mSwapMask = mStreamMask & ~resumeMask;
    }

    // Of all existing fetchers:
    // * Resume fetchers that are still needed and assign them original packet sources.
    // * Mark otherwise unneeded fetchers for removal.
#ifdef MTK_AOSP_ENHANCEMENT
	ALOGD("onChangeConfiguration3,resuming fetchers for mask 0x%08x", resumeMask);
#else
    ALOGV("resuming fetchers for mask 0x%08x", resumeMask);
#endif
    for (size_t i = 0; i < mFetcherInfos.size(); ++i) {
        const AString &uri = mFetcherInfos.keyAt(i);

        sp<AnotherPacketSource> sources[kMaxStreams];
        for (size_t j = 0; j < kMaxStreams; ++j) {
            if ((resumeMask & indexToType(j)) && uri == mStreams[j].mUri) {
                sources[j] = mPacketSources.valueFor(indexToType(j));

                if (j != kSubtitleIndex) {
#ifdef MTK_AOSP_ENHANCEMENT
					ALOGD("queueing dummy discontinuity for stream type %d", indexToType(j));

#else
                    ALOGV("queueing dummy discontinuity for stream type %d", indexToType(j));
#endif
                    sp<AnotherPacketSource> discontinuityQueue;
                    discontinuityQueue = mDiscontinuities.valueFor(indexToType(j));
                    discontinuityQueue->queueDiscontinuity(
                            ATSParser::DISCONTINUITY_NONE,
                            NULL,
                            true);
                }
            }
        }

        FetcherInfo &info = mFetcherInfos.editValueAt(i);
        if (sources[kAudioIndex] != NULL || sources[kVideoIndex] != NULL
                || sources[kSubtitleIndex] != NULL) {
            info.mFetcher->startAsync(
                    sources[kAudioIndex], sources[kVideoIndex], sources[kSubtitleIndex]);
        } else {
            info.mToBeRemoved = true;
        }
    }

    // streamMask now only contains the types that need a new fetcher created.

    if (streamMask != 0) {
#ifdef MTK_AOSP_ENHANCEMENT
		ALOGD("onChangeConfiguration3,creating new fetchers for mask 0x%08x", streamMask);
#else
        ALOGV("creating new fetchers for mask 0x%08x", streamMask);
#endif
    }

    // Find out when the original fetchers have buffered up to and start the new fetchers
    // at a later timestamp.
    for (size_t i = 0; i < kMaxStreams; i++) {
        if (!(indexToType(i) & streamMask)) {
            continue;
        }

        AString uri;
        uri = switching ? mStreams[i].mNewUri : mStreams[i].mUri;

        sp<PlaylistFetcher> fetcher = addFetcher(uri.c_str());
        CHECK(fetcher != NULL);

        int32_t latestSeq = -1;
        int64_t startTimeUs = -1;
        int64_t segmentStartTimeUs = -1ll;
        int32_t discontinuitySeq = -1;
        sp<AnotherPacketSource> sources[kMaxStreams];

        if (i == kSubtitleIndex) {
            segmentStartTimeUs = latestMediaSegmentStartTimeUs();
        }

        // TRICKY: looping from i as earlier streams are already removed from streamMask
        for (size_t j = i; j < kMaxStreams; ++j) {
            const AString &streamUri = switching ? mStreams[j].mNewUri : mStreams[j].mUri;
            if ((streamMask & indexToType(j)) && uri == streamUri) {
                sources[j] = mPacketSources.valueFor(indexToType(j));

                if (timeUs >= 0) {
                    ALOGD("rock clear packetsource %d", indexToType(j));
                    sources[j]->clear();
                    startTimeUs = timeUs;

                    sp<AnotherPacketSource> discontinuityQueue;
                    sp<AMessage> extra = new AMessage;
                    extra->setInt64("timeUs", timeUs);
                    discontinuityQueue = mDiscontinuities.valueFor(indexToType(j));
                    ALOGD("rock signal discontinuity %lld us", timeUs);
                    discontinuityQueue->queueDiscontinuity(
                            ATSParser::DISCONTINUITY_TIME, extra, true);
                } else {
                    int32_t type;
                    int64_t srcSegmentStartTimeUs;
                    sp<AMessage> meta;
                    if (pickTrack) {
                        // selecting
                        meta = sources[j]->getLatestDequeuedMeta();
                    } else {
                        // adapting
                        meta = sources[j]->getLatestEnqueuedMeta();
                    }

                    if (meta != NULL && !meta->findInt32("discontinuity", &type)) {
                        int64_t tmpUs;
                        int64_t tmpSegmentUs;

                        CHECK(meta->findInt64("timeUs", &tmpUs));
                        CHECK(meta->findInt64("segmentStartTimeUs", &tmpSegmentUs));
                        if (startTimeUs < 0 || tmpSegmentUs < segmentStartTimeUs) {
                            startTimeUs = tmpUs;
                            segmentStartTimeUs = tmpSegmentUs;
                        } else if (tmpSegmentUs == segmentStartTimeUs && tmpUs < startTimeUs) {
                            startTimeUs = tmpUs;
                        }

                        int32_t seq;
                        CHECK(meta->findInt32("discontinuitySeq", &seq));
                        if (discontinuitySeq < 0 || seq < discontinuitySeq) {
                            discontinuitySeq = seq;
                        }
                    }

                    if (pickTrack) {
                        // selecting track, queue discontinuities before content
                        sources[j]->clear();
                        if (j == kSubtitleIndex) {
                            break;
                        }
                        sp<AnotherPacketSource> discontinuityQueue;
                        discontinuityQueue = mDiscontinuities.valueFor(indexToType(j));
                        discontinuityQueue->queueDiscontinuity(
                                ATSParser::DISCONTINUITY_FORMATCHANGE, NULL, true);
                    } else {
                        // adapting, queue discontinuities after resume
                        sources[j] = mPacketSources2.valueFor(indexToType(j));
                        sources[j]->clear();
                        uint32_t extraStreams = mNewStreamMask & (~mStreamMask);
                        if (extraStreams & indexToType(j)) {
                            sources[j]->queueAccessUnit(createFormatChangeBuffer(/*swap*/ false));
                        }
                    }
                }

                streamMask &= ~indexToType(j);
            }
        }

        fetcher->startAsync(
                sources[kAudioIndex],
                sources[kVideoIndex],
                sources[kSubtitleIndex],
                startTimeUs < 0 ? mLastSeekTimeUs : startTimeUs,
                segmentStartTimeUs,
                discontinuitySeq,
                switching);
    }

    // All fetchers have now been started, the configuration change
    // has completed.

    cancelCheckBandwidthEvent();
    scheduleCheckBandwidthEvent();
#ifdef MTK_AOSP_ENHANCEMENT
	ALOGD("XXX configuration change completed.");
#else
    ALOGV("XXX configuration change completed.");
#endif
    mReconfigurationInProgress = false;
    if (switching) {
        mSwitchInProgress = true;
    } else {
        mStreamMask = mNewStreamMask;
    }

    if (mDisconnectReplyID != 0) {
        finishDisconnect();
    }
}

void LiveSession::onSwapped(const sp<AMessage> &msg) {
    int32_t switchGeneration;
    CHECK(msg->findInt32("switchGeneration", &switchGeneration));
    if (switchGeneration != mSwitchGeneration) {
        return;
    }

    int32_t stream;
    CHECK(msg->findInt32("stream", &stream));

    ssize_t idx = typeToIndex(stream);
    CHECK(idx >= 0);
    if ((mNewStreamMask & stream) && mStreams[idx].mNewUri.empty()) {
        ALOGW("swapping stream type %d %s to empty stream", stream, mStreams[idx].mUri.c_str());
    }
    mStreams[idx].mUri = mStreams[idx].mNewUri;
    mStreams[idx].mNewUri.clear();

    mSwapMask &= ~stream;
    if (mSwapMask != 0) {
        return;
    }

    // Check if new variant contains extra streams.
    uint32_t extraStreams = mNewStreamMask & (~mStreamMask);
    while (extraStreams) {
        StreamType extraStream = (StreamType) (extraStreams & ~(extraStreams - 1));
        swapPacketSource(extraStream);
        extraStreams &= ~extraStream;

        idx = typeToIndex(extraStream);
        CHECK(idx >= 0);
        if (mStreams[idx].mNewUri.empty()) {
            ALOGW("swapping extra stream type %d %s to empty stream",
                    extraStream, mStreams[idx].mUri.c_str());
        }
        mStreams[idx].mUri = mStreams[idx].mNewUri;
        mStreams[idx].mNewUri.clear();
    }

    tryToFinishBandwidthSwitch();
}

void LiveSession::onCheckSwitchDown() {
    if (mSwitchDownMonitor == NULL) {
        return;
    }

    if (mSwitchInProgress || mReconfigurationInProgress) {
        ALOGV("Switch/Reconfig in progress, defer switch down");
        mSwitchDownMonitor->post(1000000ll);
        return;
    }

    for (size_t i = 0; i < kMaxStreams; ++i) {
        int32_t targetDuration;
        sp<AnotherPacketSource> packetSource = mPacketSources.valueFor(indexToType(i));
        sp<AMessage> meta = packetSource->getLatestDequeuedMeta();

        if (meta != NULL && meta->findInt32("targetDuration", &targetDuration) ) {
            int64_t bufferedDurationUs = packetSource->getEstimatedDurationUs();
            int64_t targetDurationUs = targetDuration * 1000000ll;

            if (bufferedDurationUs < targetDurationUs / 3) {
                (new AMessage(kWhatSwitchDown, id()))->post();
                break;
            }
        }
    }

    mSwitchDownMonitor->post(1000000ll);
}

void LiveSession::onSwitchDown() {
    if (mReconfigurationInProgress || mSwitchInProgress || mCurBandwidthIndex == 0) {
        return;
    }

    ssize_t bandwidthIndex = getBandwidthIndex();
    if (bandwidthIndex < mCurBandwidthIndex) {
        changeConfiguration(-1, bandwidthIndex, false);
        return;
    }

}

// Mark switch done when:
//   1. all old buffers are swapped out
void LiveSession::tryToFinishBandwidthSwitch() {
    if (!mSwitchInProgress) {
        return;
    }

    bool needToRemoveFetchers = false;
    for (size_t i = 0; i < mFetcherInfos.size(); ++i) {
        if (mFetcherInfos.valueAt(i).mToBeRemoved) {
            needToRemoveFetchers = true;
            break;
        }
    }

    if (!needToRemoveFetchers && mSwapMask == 0) {
        ALOGI("mSwitchInProgress = false");
        mStreamMask = mNewStreamMask;
        mSwitchInProgress = false;
#ifdef MTK_AOSP_ENHANCEMENT
        //for timing issue
        //video/audio-->audio only stream
        //try to let alltrackspresent called before addfetcher(new fetchers)
        //otherwise streamMask(count new fetcher's streamMask in) is incorrect.
        cancelCheckBandwidthEvent();
        scheduleCheckBandwidthEvent();
#endif
    }
}

void LiveSession::scheduleCheckBandwidthEvent() {
    sp<AMessage> msg = new AMessage(kWhatCheckBandwidth, id());
    msg->setInt32("generation", mCheckBandwidthGeneration);
    msg->post(10000000ll);
}

void LiveSession::cancelCheckBandwidthEvent() {
    ++mCheckBandwidthGeneration;
}

void LiveSession::cancelBandwidthSwitch() {
    Mutex::Autolock lock(mSwapMutex);
    mSwitchGeneration++;
    mSwitchInProgress = false;
    mSwapMask = 0;

    for (size_t i = 0; i < mFetcherInfos.size(); ++i) {
        FetcherInfo& info = mFetcherInfos.editValueAt(i);
        if (info.mToBeRemoved) {
            info.mToBeRemoved = false;
        }
    }

    for (size_t i = 0; i < kMaxStreams; ++i) {
        if (!mStreams[i].mNewUri.empty()) {
            ssize_t j = mFetcherInfos.indexOfKey(mStreams[i].mNewUri);
            if (j < 0) {
                mStreams[i].mNewUri.clear();
                continue;
            }

            const FetcherInfo &info = mFetcherInfos.valueAt(j);
            ALOGD("stop and delete candidata PLF Uri %s", mStreams[i].mNewUri.c_str());
            info.mFetcher->stopAsync();
            mFetcherInfos.removeItemsAt(j);
            mStreams[i].mNewUri.clear();
        }
    }
}

bool LiveSession::canSwitchBandwidthTo(size_t bandwidthIndex) {
#ifdef MTK_AOSP_ENHANCEMENT
    if (mReconfigurationInProgress || mSwitchInProgress || mInPreparationPhase || mLastDequeuedTimeUs < 0/*avoid bandwidth switch before first seg download after seek*/) {
#else
    if (mReconfigurationInProgress || mSwitchInProgress) {
#endif
        return false;
    }

    if (mCurBandwidthIndex < 0) {
        return true;
    }

    if (bandwidthIndex == (size_t)mCurBandwidthIndex) {
        return false;
    } else if (bandwidthIndex > (size_t)mCurBandwidthIndex) {
#ifdef MTK_AOSP_ENHANCEMENT
        return canSwitchUp(true);//bandwidth upwards
#else
        return canSwitchUp();
#endif
    } else {
#ifdef MTK_AOSP_ENHANCEMENT
        return canSwitchUp(false);//bandwidth downwards
#else        
        return true;
#endif        
    }
}

void LiveSession::onCheckBandwidth(const sp<AMessage> &msg) {
    size_t bandwidthIndex = getBandwidthIndex(); 
#ifdef MTK_AOSP_ENHANCEMENT
		ALOGI("onCheckBandwidth,getBandwidthIndex = %d",bandwidthIndex);
#endif 
    if (canSwitchBandwidthTo(bandwidthIndex)) {
        changeConfiguration(-1ll /* timeUs */, bandwidthIndex);
    } else {
        // Come back and check again 10 seconds later in case there is nothing to do now.
        // If we DO change configuration, once that completes it'll schedule a new
        // check bandwidth event with an incremented mCheckBandwidthGeneration.
        msg->post(10000000ll);
    }
}

void LiveSession::postPrepared(status_t err) {
    ALOGD("postPrepared %d", err);
    CHECK(mInPreparationPhase);

    sp<AMessage> notify = mNotify->dup();
    if (err == OK || err == ERROR_END_OF_STREAM) {
        notify->setInt32("what", kWhatPrepared);
    } else {
        notify->setInt32("what", kWhatPreparationFailed);
        notify->setInt32("err", err);
    }

    notify->post();

    mInPreparationPhase = false;

    mSwitchDownMonitor = new AMessage(kWhatCheckSwitchDown, id());
    mSwitchDownMonitor->post();
}
#ifdef MTK_AOSP_ENHANCEMENT
void LiveSession::checkBufferingIfNecessary(bool bufferingFlag) {
    if(bufferingFlag && (mIsBuffering == false)) { //buffering start
        sp<AMessage> notify = mNotify->dup();
        notify->setInt32("what", kWhatBufferingStart);
        notify->post();
        mIsBuffering = true;
        ALOGD("buffering start, inform App");
    }
    else if(!bufferingFlag && (mIsBuffering == true)) { //buffering end
            sp<AMessage> notify = mNotify->dup();
            notify->setInt32("what", kWhatBufferingEnd);
            notify->post();
            mIsBuffering = false;
            ALOGD("buffering end");
    }
}

bool LiveSession::allTracksPresent() {
    Mutex::Autolock autoLock(mLock);

    if(mReconfigurationInProgress) {
        return false;
    }

    uint32_t streamMask = 0;
    for (size_t i = 0; i < mFetcherInfos.size(); ++i) {
        streamMask |= mFetcherInfos.valueAt(i).mFetcher->getStreamMask();
        //ALOGD("fetcher:%d,streamMask:%d",i,mFetcherInfos.valueAt(i).mFetcher->getStreamMask());
    }

    bool allFetchersPrepared = true;
    if(mFetcherInfos.size() == 0)
        allFetchersPrepared = false;
    else {
        for (size_t i = 0; i < mFetcherInfos.size(); ++i) {
            if (!mFetcherInfos.valueAt(i).mIsPrepared) {
		   ALOGD("mIsPrepared is false , i = %d", i);
                allFetchersPrepared = false;
                break;
            }
        }
    }
    
    if (streamMask == 0) {
        if(allFetchersPrepared) {//error media file?!
            return true;
        }
        return false;
    }else{
        if(allFetchersPrepared) { // for suspend then seek to end, can't exit player.
           return true;
	}
    }

    if (streamMask & STREAMTYPE_AUDIO) {
        sp<AnotherPacketSource> packetSource = mPacketSources.valueFor(STREAMTYPE_AUDIO);
        sp<MetaData> meta = packetSource->getFormat();
        if(meta == NULL) {
            return false;
        }
    }
    
    if (streamMask & STREAMTYPE_VIDEO) {
        sp<AnotherPacketSource> packetSource = mPacketSources.valueFor(STREAMTYPE_VIDEO);
        sp<MetaData> meta = packetSource->getFormat();
        if(meta == NULL) {
            return false;
        }
    }
/*    
    if (mStreamMask & STREAMTYPE_SUBTITLES) {
        sp<AnotherPacketSource> packetSource = mPacketSources.valueFor(STREAMTYPE_SUBTITLES);
        sp<MetaData> meta = packetSource->getFormat();
        if(meta == NULL)
            return false;
    }
 */   

    return true; 
}

void LiveSession::dumpPlaylist(sp<ABuffer> &buffer) {
    const int32_t nDumpSize = 2048;
    char dumpM3U8[nDumpSize];
    ALOGD("Playlist (size = %d) :\n", buffer->size());
    size_t dumpSize = (buffer->size() > (nDumpSize - 1)) ? (nDumpSize - 1) : buffer->size();
    memcpy(dumpM3U8, buffer->data(), dumpSize);
    dumpM3U8[dumpSize] = '\0';
    ALOGD("%s", dumpM3U8);
    ALOGD(" %s", ((buffer->size() < (nDumpSize - 1)) ? " " : "trunked because larger than dumpsize"));
}

void LiveSession::onPictureReceived(const sp<AMessage> &msg) {
		ALOGI("onPictureReceived");
    sp<ABuffer> metabuffer;
    CHECK(msg->findBuffer("buffer", &metabuffer));         

    sp<AMessage> notify = mNotify->dup();
    notify->setInt32("what", kWhatPicture);
    notify->setBuffer("buffer", metabuffer);
    notify->post();
}

ssize_t LiveSession::pickBandwidthIndex(int32_t bandwidthBps) {
    ssize_t index = -1;
    //for cpu schedule.
    int32_t bandwidthBps_down = bandwidthBps * 8 / 10;
    //bandwidth decrease
    if(mBandwidthItems.itemAt(mCurBandwidthIndex).mBandwidth > (size_t)bandwidthBps_down) {
        index = mCurBandwidthIndex;
        while (index > 0 && mBandwidthItems.itemAt(index).mBandwidth > (size_t)bandwidthBps_down) {
            --index;
        }
    }
    //bandwidth increase
    else{// if(mBandwidthItems.itemAt(mCurBandwidthIndex).mBandwidth <= (size_t)bandwidthBps_down) {
        index = mBandwidthItems.size() - 1;
        while (index > mCurBandwidthIndex && (mBandwidthItems.itemAt(index).mBandwidth * 130/100) > (size_t)bandwidthBps) {
            --index;
        }
    }
    return index;
}

status_t LiveSession::getTextStreamFormat(sp<AMessage> *format) {
    if (!(mStreamMask & STREAMTYPE_SUBTITLES)) {
        return UNKNOWN_ERROR;
    }

    sp<AnotherPacketSource> packetSource = mPacketSources.valueFor(STREAMTYPE_SUBTITLES);

    sp<MetaData> meta = packetSource->getFormat();

    if (meta == NULL) {
        return -EAGAIN;
    }

    return convertMetaDataToMessage(meta, format);
}
#endif
}  // namespace android
