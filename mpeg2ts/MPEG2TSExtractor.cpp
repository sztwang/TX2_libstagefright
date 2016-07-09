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
#define LOG_TAG "MPEG2TSExtractor"
#include <utils/Log.h>

#include "include/MPEG2TSExtractor.h"
#include "include/NuCachedSource2.h"

#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/DataSource.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/MediaSource.h>
#include <media/stagefright/MetaData.h>
#include <utils/String8.h>

#include "AnotherPacketSource.h"
#include "ATSParser.h"

#ifdef MTK_AOSP_ENHANCEMENT
const static int64_t kMaxPTSTimeOutUs = 3000000LL;  //  handle find Duration for ANR
#endif
#ifdef MTK_AOSP_ENHANCEMENT
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/MediaBuffer.h>
#include "include/avc_utils.h"
#define SUPPORT_M2TS
//#undef SUPPORT_M2TS
#endif
namespace android {

static const size_t kTSPacketSize = 188;
#if defined(MTK_AOSP_ENHANCEMENT) && (defined(SUPPORT_M2TS))
static const size_t kM2TSPacketSize = 192;
static size_t kFillPacketSize = 188;
#endif //#if !defined(ANDROID_DEFAULT_CODE) && (defined(SUPPORT_M2TS))

struct MPEG2TSSource : public MediaSource {
    MPEG2TSSource(
            const sp<MPEG2TSExtractor> &extractor,
            const sp<AnotherPacketSource> &impl,
            bool seekable);

    virtual status_t start(MetaData *params = NULL);
    virtual status_t stop();
    virtual sp<MetaData> getFormat();

    virtual status_t read(
            MediaBuffer **buffer, const ReadOptions *options = NULL);

private:
    sp<MPEG2TSExtractor> mExtractor;
    sp<AnotherPacketSource> mImpl;

    // If there are both audio and video streams, only the video stream
    // will be seekable, otherwise the single stream will be seekable.
    bool mSeekable;
#ifdef MTK_AOSP_ENHANCEMENT
    bool mIsVideo;
    List<sp<ABuffer> >mLeftBuffer;    // multi-NAL cut to signal
    bool mWantsNALFragments;
    status_t cutBufferToNAL(MediaBuffer * buffer);
#endif

    DISALLOW_EVIL_CONSTRUCTORS(MPEG2TSSource);
};

MPEG2TSSource::MPEG2TSSource(
        const sp<MPEG2TSExtractor> &extractor,
        const sp<AnotherPacketSource> &impl,
        bool seekable)
    : mExtractor(extractor),
      mImpl(impl),
#ifdef MTK_AOSP_ENHANCEMENT
      mIsVideo(true),
      mWantsNALFragments(false),
#endif
      mSeekable(seekable) {
}

status_t MPEG2TSSource::start(MetaData *params) {
#ifdef MTK_AOSP_ENHANCEMENT
    int32_t val;
    if (params && params->findInt32(kKeyWantsNALFragments, &val)
        && val != 0) {
        ALOGI("wants nal fragments");
        mWantsNALFragments = true;
    } else {
        mWantsNALFragments = false;
    }
#endif
    return mImpl->start(params);
}

status_t MPEG2TSSource::stop() {
#ifdef MTK_AOSP_ENHANCEMENT
    ALOGD("Stop Video=%d track", mIsVideo);
    if (mIsVideo == true)
        mExtractor->setVideoState(true);
#endif
    return mImpl->stop();
}

#ifdef MTK_AOSP_ENHANCEMENT
sp<MetaData> MPEG2TSSource::getFormat() {
    if (mImpl == NULL) {
        return NULL;
    } else {
        sp<MetaData> meta = mImpl->getFormat();
        if (meta == NULL)
            return NULL;
    }

    sp<MetaData> meta = mImpl->getFormat();

    if (meta == NULL)
        return NULL;

    int64_t durationUs;

 	if (!meta->findInt64(kKeyDuration,&durationUs)) {    
		meta->setInt64(kKeyDuration, mExtractor->getDurationUs());	
	}

    const char *mime;
    CHECK(meta->findCString(kKeyMIMEType, &mime));

    if (!strncasecmp("audio/", mime, 6)) {
        mIsVideo = false;
#ifdef MTK_SUBTITLE_SUPPORT
    } else if (!strncasecmp("text/", mime, 5)) {
#endif
    } else {
        CHECK(!strncasecmp("video/", mime, 6));
        mIsVideo = true;
#ifdef MTK_AOSP_ENHANCEMENT
	meta->setPointer('anps', mImpl.get());
#endif
    }

    return meta;
}
#else
sp<MetaData> MPEG2TSSource::getFormat() {
    return mImpl->getFormat();
}
#endif

status_t MPEG2TSSource::read(
        MediaBuffer **out, const ReadOptions *options) {
    *out = NULL;

    int64_t seekTimeUs;
    ReadOptions::SeekMode seekMode;
#ifdef MTK_AOSP_ENHANCEMENT
    if (options && options->getSeekTo(&seekTimeUs, &seekMode)) {
        if (mExtractor->getVideoState() && !mIsVideo && !mSeekable) {
            mSeekable = true;
            ALOGE("video Audio can seek now");
        }
        if (mSeekable) {
            mExtractor->seekTo(seekTimeUs);
			mImpl->queueDiscontinuity(ATSParser::DISCONTINUITY_NONE, NULL, false /* discard */ );
        }
    }
    // if has left buffer return; if seek, clear buffer
    if (mWantsNALFragments) {
        if (options != NULL) {
            mLeftBuffer.clear();
        } else if (!mLeftBuffer.empty()) {
            sp<ABuffer> buffer = *mLeftBuffer.begin();
            mLeftBuffer.erase(mLeftBuffer.begin());

            int64_t timeUs;
            CHECK(buffer->meta()->findInt64("timeUs", &timeUs));

            MediaBuffer *mediaBuffer = new MediaBuffer(buffer);
            mediaBuffer->meta_data()->setInt64(kKeyTime, timeUs);

            int64_t seekTimeUs;
            ReadOptions::SeekMode seekMode;
            if (options && options->getSeekTo(&seekTimeUs, &seekMode)) {
                mediaBuffer->meta_data()->setInt64(kKeyTargetTime,
                                                   seekTimeUs);
            }
            *out = mediaBuffer;
            return OK;
        }
    }
#else
    if (mSeekable && options && options->getSeekTo(&seekTimeUs, &seekMode)) {
        return ERROR_UNSUPPORTED;
    }
#endif

    status_t finalResult;
#ifdef MTK_AOSP_ENHANCEMENT
    while (!mImpl->hasBufferAvailable(&finalResult)
           || mExtractor->getSeeking()) {
#else
    while (!mImpl->hasBufferAvailable(&finalResult)) {
#endif
        if (finalResult != OK) {

            return ERROR_END_OF_STREAM;
        }

        status_t err = mExtractor->feedMore();
        if (err != OK) {
            mImpl->signalEOS(err);
        }
    }
#ifndef MTK_AOSP_ENHANCEMENT

    return mImpl->read(out, options);
#else
    if (!mWantsNALFragments) {
#ifdef MTK_AUDIO_CHANGE_SUPPORT
        status_t ret;
        ret = mImpl->read(out, options);
        if(ret == OK){
            sp<MetaData> meta = mImpl->getFormat();
            const char *mime;
            CHECK(meta->findCString(kKeyMIMEType, &mime));
            if (!strncasecmp("audio/", mime, 6)) {
                MediaBuffer *buffer = *out;
                int64_t timeUs;
                CHECK(buffer->meta_data()->findInt64(kKeyTime, &timeUs));
                if(timeUs >=0){
                   mExtractor->consumeData(mImpl,timeUs,true);
                }
            }
        }
        return ret;
#else
        return mImpl->read(out, options);
#endif
    } else {
        status_t ret;
        if ((ret = mImpl->read(out, options)) != OK) {
            ALOGI("mImpl->read not OK");
            return ret;
        }
        MediaBuffer *buffers = *out;
        cutBufferToNAL(buffers);
        buffers->release();

        // if left buffer is not empty, return buffer
        if (!mLeftBuffer.empty()) {
            sp<ABuffer> buffer = *mLeftBuffer.begin();
            mLeftBuffer.erase(mLeftBuffer.begin());

            int64_t timeUs;
            CHECK(buffer->meta()->findInt64("timeUs", &timeUs));

            MediaBuffer *mediaBuffer = new MediaBuffer(buffer);
            mediaBuffer->meta_data()->setInt64(kKeyTime, timeUs);

            int64_t seekTimeUs;
            ReadOptions::SeekMode seekMode;
            if (options && options->getSeekTo(&seekTimeUs, &seekMode)) {
                mediaBuffer->meta_data()->setInt64(kKeyTargetTime,
                                                   seekTimeUs);
            }
            *out = mediaBuffer;
#ifdef MTK_AUDIO_CHANGE_SUPPORT
            sp<MetaData> meta = mImpl->getFormat();
            const char *mime;
            CHECK(meta->findCString(kKeyMIMEType, &mime));
            if (!strncasecmp("audio/", mime, 6)) {
                MediaBuffer *buffer = *out;
                if(timeUs >=0){
                   mExtractor->consumeData(mImpl,timeUs,true);
                }
            }
#endif
            return OK;
        } else {
            ALOGW("cut nal fail");
            return UNKNOWN_ERROR;
        }
    }
#endif

}

#ifdef MTK_AOSP_ENHANCEMENT
// cut buffer to signal NAL
status_t MPEG2TSSource::cutBufferToNAL(MediaBuffer * buffer) {
    const uint8_t *data = (uint8_t *) buffer->data() + buffer->range_offset();
    size_t size = buffer->range_length();

    int64_t timeUs = 0;
    CHECK(buffer->meta_data()->findInt64(kKeyTime, &timeUs));

    status_t err;
    const uint8_t *nalStart;
    size_t nalSize;
    while ((err =
            getNextNALUnit(&data, &size, &nalStart, &nalSize, true)) == OK) {
        CHECK_GT(nalSize, 0u);
        sp<ABuffer> nalBuf = new ABuffer(nalSize);
        memcpy(nalBuf->data(), nalStart, nalSize);
        nalBuf->meta()->setInt64("timeUs", timeUs);
        mLeftBuffer.push_back(nalBuf);
    }
    return OK;
}
#endif
////////////////////////////////////////////////////////////////////////////////

#ifdef MTK_AOSP_ENHANCEMENT
int32_t findSyncCode(const void *data, size_t size) {
    uint32_t i = 0;
    for (i = 0; i < size; i++) {
        if (((uint8_t *) data)[i] == 0x47u)
            return i;
    }
    return -1;
}
#endif
MPEG2TSExtractor::MPEG2TSExtractor(const sp<DataSource> &source)
    : mDataSource(source),
#ifdef MTK_AOSP_ENHANCEMENT
    mParser(new ATSParser(0x40000000)),
    mDurationUs(0),
    mSeekTimeUs(0),
    mSeeking(false),
    End_OF_FILE(false),//cherry
    mSeekingOffset(0),
    mFileSize(0),
    mMinOffset(0),
    mMaxOffset(0),
    mMaxcount(0),
    mVideoUnSupportedByDecoder(false),
    mOffsetPAT(0),
#else
      mParser(new ATSParser),
#endif
      mOffset(0) {
#ifdef MTK_AOSP_ENHANCEMENT
    ALOGD("=====================================\n");
    ALOGD("[MPEG2TS Playback capability info]\A3\BA\n");
    ALOGD("=====================================\n");
    ALOGD("Resolution = \"[(8,8) ~ (1280\A3\AC720)]\" \n");
    ALOGD("Support Codec = \"Video:MPEG4, H264, MPEG1,MPEG2 ; Audio: AAC,MP3\" \n");
    ALOGD("Profile_Level = \"MPEG4: ASP ;  H264: Baseline/3.1, Main/3.1,High/3.1\" \n");
    ALOGD("Max frameRate =  120fps \n");
    ALOGD("Max Bitrate  = H264: 2Mbps  (720P@30fps) ; MPEG4/H263: 4Mbps (720P@30fps)\n");
    ALOGD("=====================================\n");
    if (!(mDataSource->flags() & DataSource::kIsCachingDataSource)) {   // http streaming do not get duration
    bool retb=true;	
    off64_t StartOffset = 0;
    off64_t NewOffset = 0;
    
    if (findSyncWord(source,StartOffset,10 * kTSPacketSize,kTSPacketSize,NewOffset)) {
        ALOGD("MPEG2TSExtractor:this is ts file\n");
        kFillPacketSize = kTSPacketSize;
    }else {
        retb = false;
    }

    if(!retb){
        StartOffset = 0;
        NewOffset = 0;
        retb = true;
        if (findSyncWord(source,StartOffset,10 * kM2TSPacketSize,kM2TSPacketSize,NewOffset)) {
            ALOGD("MPEG2TSExtractor:this is m2ts file\n");
            kFillPacketSize = kM2TSPacketSize;
        } else {
            retb = false;
            ALOGE("MPEG2TSExtractor: it is not a ts/m2ts file!!!");
        }
    }
        status_t err = parseMaxPTS();   //parse all the TS packet of this file?
        if (err != OK) {
            return;
        }

#ifdef MTK_AUDIO_CHANGE_SUPPORT
	//reset the firstPTSisvalid of program which is not playing
	mParser->setFirstPTSIsValid();
#endif
    }
    //[qian]may be we should add the seek table creation this section
    //when 2st parse whole file
    ALOGD("MPEG2TSExtractor: after parseMaxPTS  mOffset=%lld", mOffset);
#endif
    init();
}
#ifdef MTK_AOSP_ENHANCEMENT
#ifdef MTK_AUDIO_CHANGE_SUPPORT
bool MPEG2TSExtractor::needRemoveData(sp<AnotherPacketSource> impl,int64_t timeUs,bool isAudio){
    status_t finalResult ;
    if(impl->hasBufferAvailable(&finalResult)){
       sp<AMessage> firstMeta = impl->getLatestDequeuedMeta();
       if(firstMeta == NULL){
           int64_t timeDur = impl->getEstimatedDurationUs();
           if(timeDur > 2000000){ //2s
               ALOGD("dur :%lld >2s",timeDur);
               return true;
           }else{
               ALOGD("dur :%lld ",timeDur);
               return false;
           }

       }
       int64_t timeUs2;
       if((firstMeta !=NULL) && firstMeta->findInt64("timeUs", &timeUs2)){
          if(timeUs2 != -1 &&timeUs2 + 500000 < timeUs  ){
              ALOGV("timeUS:%lld, timeUs2:%lld",timeUs,timeUs2);
              return true;
          }
       }
    }
    return false;
}
bool MPEG2TSExtractor::consumeData(sp<AnotherPacketSource> impl,int64_t timeUs,bool isAudio){
    ALOGV("consumeData++ track num:%d, time:%lld",mSourceImpls.size(),timeUs);
    sp<MetaData> meta = impl->getFormat();
    const char *mime0;
    for (size_t index = 0; index < mSourceImpls.size(); index++) {
        sp<AnotherPacketSource> impl1 = mSourceImpls.editItemAt(index);
        if(impl1 != impl){// can compare?
            sp<MetaData> meta2 = impl1->getFormat();
            if(meta2 == NULL){
                ALOGD("meta2 null ");
                continue;
            }
            const char *mime;
            CHECK(meta2->findCString('mime', &mime));
            if (!strncasecmp("audio/", mime, 6)) {
                status_t finalResult;
                while(impl1->hasBufferAvailable(&finalResult)){
                    if(needRemoveData(impl1,timeUs,true)){
                        MediaBuffer *out;
                        MediaSource::ReadOptions options;
                        status_t err1 = impl1->read(&out,&options);
                        if(err1 == OK){
                           MediaBuffer *buffer = out;
                           int64_t  removetimeUs;
                           CHECK(buffer->meta_data()->findInt64(kKeyTime, &removetimeUs));
                           ALOGD("remove time:%lld,play time :%lld, index:%d,mime:%s",removetimeUs,timeUs,index,mime);
			   buffer->release();
                        }else{
                            ALOGD("read err:%d",err1);
                        }
                     }else{
                          ALOGV("no need remove");
                          break;
                     }
                }
            }
        }
    }
    ALOGV("consumeData--");
    return true;
}
#endif
#endif
size_t MPEG2TSExtractor::countTracks() {




    return mSourceImpls.size();
}

sp<MediaSource> MPEG2TSExtractor::getTrack(size_t index) {
    if (index >= mSourceImpls.size()) {
        return NULL;
    }

    bool seekable = true;
    if (mSourceImpls.size() > 1) {
#ifndef MTK_AUDIO_CHANGE_SUPPORT
        CHECK_EQ(mSourceImpls.size(), 2u);
#endif
        sp<MetaData> meta = mSourceImpls.editItemAt(index)->getFormat();
        const char *mime;
        CHECK(meta->findCString(kKeyMIMEType, &mime));

        if (!strncasecmp("audio/", mime, 6)) {
            seekable = false;
        }
#ifdef MTK_SUBTITLE_SUPPORT
        if (!strncasecmp("text/", mime, 5)){
            seekable = false;
        }
#endif
    }

    return new MPEG2TSSource(this, mSourceImpls.editItemAt(index), seekable);
}

sp<MetaData> MPEG2TSExtractor::getTrackMetaData(
        size_t index, uint32_t /* flags */) {
#ifdef MTK_AOSP_ENHANCEMENT
    if (index >= mSourceImpls.size())
        return NULL;

    sp<MetaData> meta = mSourceImpls.editItemAt(index)->getFormat();

    int64_t durationUs = (int64_t) (getDurationUs());
    if (durationUs < 0 || NULL == meta.get())
        return NULL;

    if (meta != NULL) {
        meta->setInt64(kKeyDuration, getDurationUs());
    }
#endif
    return index < mSourceImpls.size()
        ? mSourceImpls.editItemAt(index)->getFormat() : NULL;
}

sp<MetaData> MPEG2TSExtractor::getMetaData() {
    sp<MetaData> meta = new MetaData;
#ifdef MTK_AOSP_ENHANCEMENT
    bool hasVideo = false;
    for (int index = 0; index < mSourceImpls.size(); index++) {
        sp<MetaData> meta = mSourceImpls.editItemAt(index)->getFormat();
        const char *mime;
        CHECK(meta->findCString(kKeyMIMEType, &mime));
        if (!strncasecmp("video/", mime, 6)) {
            hasVideo = true;
        }
    }
    //[qian]can set the hasvideo to be class member, not need to read meta
    //has parsed the hasvideo value in init() funtion
    if (hasVideo) {
        meta->setCString(kKeyMIMEType, "video/mp2ts");
    } else {
        meta->setCString(kKeyMIMEType, "audio/mp2ts");
    }

    // set flag for handle the case: video too long to audio
    meta->setInt32(kKeyVideoPreCheck, 1);

#else
    meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_CONTAINER_MPEG2TS);
#endif
    return meta;
}

void MPEG2TSExtractor::init() {
    bool haveAudio = false;
    bool haveVideo = false;
    int numPacketsParsed = 0;

#ifdef MTK_AOSP_ENHANCEMENT
    mOffset = 0;
#endif
#ifdef MTK_AUDIO_CHANGE_SUPPORT
	sp<MetaData> meta;
    Vector<sp<AnotherPacketSource> > mSourceImplsTemp;
    unsigned* puStrmPidInfo = NULL;
    int i4PidNum = mParser->parsedPIDSize();
    int i4PidNumIdx = 0;
    int i4PidSourceIdx = 0;
    puStrmPidInfo = new unsigned[i4PidNum];

    for (i4PidNumIdx = 0; i4PidNumIdx < i4PidNum; i4PidNumIdx ++){
       puStrmPidInfo[i4PidNumIdx] = mParser->getParsedPID(i4PidNumIdx);
    }

	while (feedMore() == OK) {
		int i = 0;
        int i4PidIdx = 0;

		if (!haveVideo) {
            sp<AnotherPacketSource> impl =
                (AnotherPacketSource *)mParser->getSource(
                        ATSParser::VIDEO).get();

            if (impl != NULL) {
                haveVideo = true;
            }
        }
        if (!haveAudio) {
            sp<AnotherPacketSource> impl =
                (AnotherPacketSource *)mParser->getSource(
                        ATSParser::AUDIO).get();

            if (impl != NULL) {
                haveAudio = true;
            }
        }
		while (i < mParser->parsedPIDSize()) {

			unsigned elemPID = mParser->getParsedPID(i);
            bool hasAddSouce = false;

            for(i4PidIdx = 0; i4PidIdx < mSourceImplsTemp.size(); i4PidIdx++){
               if (elemPID == mSourceImplsTemp.editItemAt(i4PidIdx)->getSourcePID()){
                   hasAddSouce = true;
                   break;
               }
            }
            if(!hasAddSouce) {
				sp<AnotherPacketSource> impl =
					(AnotherPacketSource *)mParser->getSource(elemPID, 0).get();

				if (impl != NULL) {
                    ALOGD("add source with PID:%x  n:%d", elemPID, numPacketsParsed);
                    impl->setSourcePID(elemPID);
                    mSourceImplsTemp.push(impl);
                    //mParser->removeParsedPID(i);
                    //i = 0;//reset index
                    //ALOGD("isParsedPIDEmpty:%d, size:%d",mParser->isParsedPIDEmpty(),mSourceImpls.size());
				}
			}
			i++;
		}

        if((mParser->parsedPIDSize() == mSourceImplsTemp.size()) && mSourceImplsTemp.size() != 0) {
            ALOGD("find all source n:%d",numPacketsParsed);
			break;
		}
		if (++numPacketsParsed > 30000) {
			ALOGD("can not fine the source data %d",numPacketsParsed);
			break;
		}
	}
    if (mSourceImplsTemp.size() != 0){
        for (i4PidNumIdx = 0; i4PidNumIdx < i4PidNum; i4PidNumIdx ++){
           for(i4PidSourceIdx = 0; i4PidSourceIdx<mSourceImplsTemp.size(); i4PidSourceIdx++){
               if (puStrmPidInfo[i4PidNumIdx] == mSourceImplsTemp.editItemAt(i4PidSourceIdx)->getSourcePID()){
                   mSourceImpls.push(mSourceImplsTemp.editItemAt(i4PidSourceIdx));
                   ALOGD("re-add source with PID:%u", puStrmPidInfo[i4PidNumIdx]);
               }
           }
        }
    }
    if (puStrmPidInfo != NULL)
    {
        free(puStrmPidInfo);
        puStrmPidInfo = NULL;
    }

	for (int i=0; i < mSourceImpls.size();i++){
		ALOGD("mSourceImpls.size():%d,implPID:0x%x",mSourceImpls.size(),mSourceImpls.editItemAt(i)->getSourcePID());
    }
#else
    while (feedMore() == OK) {
        ATSParser::SourceType type;
        if (haveAudio && haveVideo) {
            break;
        }
        if (!haveVideo) {
            sp<AnotherPacketSource> impl =
                (AnotherPacketSource *)mParser->getSource(
                        ATSParser::VIDEO).get();

            if (impl != NULL) {
                haveVideo = true;
                mSourceImpls.push(impl);
            }
        }

        if (!haveAudio) {
            sp<AnotherPacketSource> impl =
                (AnotherPacketSource *)mParser->getSource(
                        ATSParser::AUDIO).get();

            if (impl != NULL) {
                haveAudio = true;
                mSourceImpls.push(impl);
            }
        }

#ifdef MTK_AOSP_ENHANCEMENT
        if (++numPacketsParsed > 30000) {
#else
        if (++numPacketsParsed > 10000) {
#endif
            break;
        }
    }
#endif//MTK_AUDIO_CHANGE_SUPPORT

    ALOGI("haveAudio=%d, haveVideo=%d", haveAudio, haveVideo);
}

status_t MPEG2TSExtractor::feedMore() {
    Mutex::Autolock autoLock(mLock);
#ifdef MTK_AOSP_ENHANCEMENT
    if (mSeeking) {
        int64_t pts = mParser->getMaxPTS(); //[qian] get the max pts in the had read data
		if(End_OF_FILE){
			ALOGE("seek to End_OF_FILE last time");
			mOffset=
			   (off64_t) ((((mMinOffset +
							  mMaxOffset) / 2) / kFillPacketSize) *
						   kFillPacketSize);
			
			mSeekingOffset = mOffset;
			End_OF_FILE=false;
		
		}//to solve the problem that when seek to End_OF_FILE then seek to another place ,instantly. and can not play

        if (pts > 0) {
            mMaxcount++;
            if ((pts - mSeekTimeUs < 50000 && pts - mSeekTimeUs > -50000)
                || mMinOffset == mMaxOffset || mMaxcount > 13) {
                //ALOGE("pts=%lld,mSeekTimeUs=%lld,mMaxcount=%lld,mMinOffset=0x%x,mMaxOffset=0x%x",pts/1000,mSeekTimeUs/1000,mMaxcount, mMinOffset,mMaxOffset );
                mSeeking = false;
                mParser->setDequeueState(true); //
            } else {
                mParser->signalDiscontinuity(ATSParser::DISCONTINUITY_TIME
                                             /* isSeek */ );
                if (pts < mSeekTimeUs) {
                    mMinOffset = mSeekingOffset;    //[qian], 1 enter this will begin with the mid of file

                } else {
                    mMaxOffset = mSeekingOffset;
                }
#if defined(MTK_AOSP_ENHANCEMENT) && (defined(SUPPORT_M2TS))
                mSeekingOffset =
                    (off64_t) ((((mMinOffset +
                                  mMaxOffset) / 2) / kFillPacketSize) *
                               kFillPacketSize);
#else //#if !defined(ANDROID_DEFAULT_CODE) && (defined(SUPPORT_M2TS))

                mSeekingOffset =
                    (off64_t) ((((mMinOffset +
                                  mMaxOffset) / 2) / kTSPacketSize) *
                               kTSPacketSize);
#endif //#if !defined(ANDROID_DEFAULT_CODE) && (defined(SUPPORT_M2TS))

                mOffset = mSeekingOffset;
            }
            ALOGE
                ("pts=%lld,mSeekTimeUs=%lld,mMaxcount=%lld,mOffset=%lld,mMinOffset=%lld,mMaxOffset=%lld",
                 pts / 1000, mSeekTimeUs / 1000, mMaxcount, mOffset,
                 mMinOffset, mMaxOffset);

        }
    }
#endif
#if defined(MTK_AOSP_ENHANCEMENT) && (defined(SUPPORT_M2TS))
    uint8_t packet[kFillPacketSize * 2];
    status_t retv = OK;
    ssize_t n = mDataSource->readAt(mOffset, packet, kFillPacketSize);

    if (n < (ssize_t) kFillPacketSize) {
        ALOGE(" mOffset=%lld,n =%ld", mOffset, n);
		End_OF_FILE = true;
        return (n < 0) ? (status_t) n : ERROR_END_OF_STREAM;
    }
    ALOGV("feedMore:mOffset = %lld  packet=0x%x,0x%x,0x%x,0x%x", mOffset,
          packet[0], packet[1], packet[2], packet[3]);

    mOffset += n;
    if (kFillPacketSize == kM2TSPacketSize) {

        retv = mParser->feedTSPacket(packet + 4, kFillPacketSize - 4);

        if (retv == BAD_VALUE) {
            int32_t syncOff = 0;
            syncOff = findSyncCode(packet + 4, kFillPacketSize - 4);
            if (syncOff >= 0) {
                mOffset -= n;
                mOffset += syncOff;
            }
            return OK;
        } else {
            return retv;
        }

    } else {

        retv = mParser->feedTSPacket(packet, kFillPacketSize);

        if (retv == BAD_VALUE) {
            int32_t syncOff = 0;
            syncOff = findSyncCode(packet, kFillPacketSize);
            if (syncOff >= 0) {
                mOffset -= n;
                mOffset += syncOff;
            }
            ALOGE("[TS_ERROR]correction once offset mOffset=%lld", mOffset);
            return OK;
        } else {
            return retv;
        }
    }
#else //#if !defined(ANDROID_DEFAULT_CODE) && (defined(SUPPORT_M2TS))

#ifdef MTK_AOSP_ENHANCEMENT
    uint8_t packet[kTSPacketSize];
    status_t retv = OK;

    ssize_t n = mDataSource->readAt(mOffset, packet, kTSPacketSize);

    if (n < (ssize_t) kTSPacketSize) {
        ALOGE(" mOffset=%lld,n =%ld", mOffset, n);
        return (n < 0) ? (status_t) n : ERROR_END_OF_STREAM;
    }

    ALOGV("mOffset= %lld  packet=0x%x,0x%x,0x%x,0x%x", mOffset, packet[0],
          packet[1], packet[2], packet[3]);
    mOffset += n;
    retv = mParser->feedTSPacket(packet, kTSPacketSize);

    if (retv == BAD_VALUE) {
        int32_t syncOff = 0;
        syncOff = findSyncCode(packet, kTSPacketSize);
        if (syncOff >= 0) {
            mOffset -= n;
            mOffset += syncOff;
        }
        ALOGE("[TS_ERROR]correction once offset mOffset=%lld", mOffset);
        return OK;
    } else {
        return retv;
    }
#else
    uint8_t packet[kTSPacketSize];
    ssize_t n = mDataSource->readAt(mOffset, packet, kTSPacketSize);

    if (n < (ssize_t)kTSPacketSize) {
        return (n < 0) ? (status_t)n : ERROR_END_OF_STREAM;
    }

    mOffset += n;
    return mParser->feedTSPacket(packet, kTSPacketSize);
#endif
#endif //#if !defined(ANDROID_DEFAULT_CODE) && (defined(SUPPORT_M2TS))
}

#ifdef MTK_AOSP_ENHANCEMENT

bool MPEG2TSExtractor::getSeeking() {
    return mSeeking;
}
void MPEG2TSExtractor::setVideoState(bool state) {
    mVideoUnSupportedByDecoder = state;
    ALOGE("setVideoState  mVideoUnSupportedByDecoder=%d",
          mVideoUnSupportedByDecoder);
}
bool MPEG2TSExtractor::getVideoState(void) {
    ALOGE("getVideoState  mVideoUnSupportedByDecoder=%d",
          mVideoUnSupportedByDecoder);
    return mVideoUnSupportedByDecoder;

}
#endif
#ifdef MTK_AOSP_ENHANCEMENT
bool MPEG2TSExtractor::findPAT() {
    Mutex::Autolock autoLock(mLock);

#if defined(MTK_AOSP_ENHANCEMENT) && (defined(SUPPORT_M2TS))
    uint8_t packet[kFillPacketSize];
    ssize_t n = mDataSource->readAt(mOffset, packet, kFillPacketSize);
    ALOGV("findPAT mOffset= %lld  packet=0x%x,0x%x,0x%x,0x%x", mOffset,
          packet[0], packet[1], packet[2], packet[3]);
    if (kFillPacketSize == kM2TSPacketSize) {
        return mParser->findPAT(packet + 4, kFillPacketSize - 4);
    } else {
        return mParser->findPAT(packet, kFillPacketSize);
    }
#else //#if !defined(ANDROID_DEFAULT_CODE) && (defined(SUPPORT_M2TS))

    uint8_t packet[kTSPacketSize];

    ssize_t n = mDataSource->readAt(mOffset, packet, kTSPacketSize);
    ALOGV("findPAT mOffset=0x%lld,packet=0x%x,0x%x,0x%x,0x%x", mOffset,
          packet[0], packet[1], packet[2], packet[3]);
    return mParser->findPAT(packet, kTSPacketSize);

#endif //#if !defined(ANDROID_DEFAULT_CODE) && (defined(SUPPORT_M2TS))

}

#if defined(MTK_AOSP_ENHANCEMENT) && (defined(SUPPORT_M2TS))
bool findSyncWord(const sp<DataSource> &source,off64_t StartOffset, uint64_t size, size_t PacketSize, off64_t &NewOffset) {

	uint8_t packet[PacketSize];
    off64_t Offset = StartOffset;

	ssize_t n = source->readAt(Offset, packet, PacketSize);
    ALOGV("findSyncWord mOffset= %lld  packet=0x%x,0x%x,0x%x,0x%x,0x%x",Offset,packet[0],packet[1],packet[2],packet[3],packet[4]);
        
    if(((PacketSize == kTSPacketSize) && packet[0] != 0x47) ||
        ((PacketSize == kM2TSPacketSize) && packet[4] != 0x47)){
        uint8_t packetTempS[PacketSize*3];
        uint32_t index = 0;
        for (;Offset < (StartOffset + size - 3*PacketSize);) {

            Offset = Offset + PacketSize;
            ssize_t n = source->readAt(Offset, packetTempS, PacketSize*3);
            for (index = 0; index <  PacketSize; index++) {
                if((packetTempS[index] == 0x47) && (packetTempS[index+ PacketSize] == 0x47) && (packetTempS[index+ PacketSize*2] == 0x47)){
                    break;
                    }
            }
            if(index >= PacketSize){
                //ALOGE("findSyncWord: can not find sync word");            
            }else if (index >= 0) {
                if(PacketSize == kTSPacketSize) {
                    NewOffset = Offset + index + 2 * PacketSize;
                }else {
                    NewOffset = Offset + index - 4 + 2 * PacketSize;
                }              
                ALOGD("findSyncWord mOffset= %lld  kFillPacketSize:%d packet=0x%x,0x%x,0x%x,0x%x,0x%x",NewOffset,PacketSize,packetTempS[index],packetTempS[index+1],packetTempS[index+2],packetTempS[index+3],packetTempS[index+4]);
                return true;
            }
        }
        ALOGE("findSyncWord: can not find sync word");          
    }else {
        return true;
    }
    return false;
}
#endif

status_t MPEG2TSExtractor::parseMaxPTS() {
    mDataSource->getSize(&mFileSize);
	ALOGE("mFileSize:%lld",mFileSize);
#if defined(MTK_AOSP_ENHANCEMENT) && (defined(SUPPORT_M2TS))
    off64_t counts = mFileSize / kFillPacketSize;
#else //#if !defined(ANDROID_DEFAULT_CODE) && (defined(SUPPORT_M2TS))
    off64_t counts = mFileSize / kTSPacketSize;
#endif //#if !defined(ANDROID_DEFAULT_CODE) && (defined(SUPPORT_M2TS))

#ifdef MTK_AOSP_ENHANCEMENT
    int32_t numPacketsParsed = 0;
#endif
    int64_t maxPTSStart = systemTime() / 1000;
    //really dequeue data?
    mParser->setDequeueState(false);
    //[qian]set false, when parse the ts pakect, will not exec the  main function of onPayloadData
    //only parse the PAT, PMT,PES header, save parse time

    //if (!(mParser->mFlags & TS_TIMESTAMPS_ARE_ABSOLUTE)) {
    //get first pts(pts in in PES packet)
    bool foundFirstPTS = false;
    while (feedMore() == OK) {
        if (mParser->firstPTSIsValid()) {
            ALOGD("parseMaxPTS:firstPTSIsValid, mOffset", mOffset);
            foundFirstPTS = true;
            break;
        }
#ifdef MTK_AOSP_ENHANCEMENT
        if (++numPacketsParsed > 30000)
        {
            break;
	    }
#endif 
    }
    if (!foundFirstPTS) {
#ifdef MTK_AOSP_ENHANCEMENT
        ALOGI("not found first PTS numPacketsParsed %d", numPacketsParsed);
#else
		ALOGI("not found first PTS");
#endif
        return OK;
    }
    //clear

    mParser->signalDiscontinuity(ATSParser::DISCONTINUITY_TIME /* isSeek */ );
    //}

    //get duration
    mOffsetPAT = mFileSize;
    for (off64_t i = 1; i <= counts; i++) {
        int64_t maxPTSDuration = systemTime() / 1000 - maxPTSStart;
        if (maxPTSDuration > kMaxPTSTimeOutUs) {
            ALOGD("TimeOut find PTS, start time=%lld, duration=%lld",
                  maxPTSStart, maxPTSDuration);
            return UNKNOWN_ERROR;
        }
#if defined(MTK_AOSP_ENHANCEMENT) && (defined(SUPPORT_M2TS))

		if(mOffsetPAT > 2500 * i * i * kFillPacketSize){
		    mOffsetPAT= (off64_t)(mOffsetPAT - 2500 * i * i * kFillPacketSize);
        }else {
            mOffsetPAT = 0;
        }

        mOffset = mOffsetPAT;
#else //#if !defined(ANDROID_DEFAULT_CODE) && (defined(SUPPORT_M2TS))

        mOffset = (off64_t) ((counts - i) * kTSPacketSize);
#endif //#if !defined(ANDROID_DEFAULT_CODE) && (defined(SUPPORT_M2TS))
#if defined(MTK_AOSP_ENHANCEMENT) && (defined(SUPPORT_M2TS))
		if (findSyncWord(mDataSource,mOffsetPAT,1000 * kFillPacketSize,kFillPacketSize,mOffset)) {//find last PAT
#else
        if (findPAT()) {        //find last PAT
#endif
            //start searching from the last PAT
            ALOGD("parseMaxPTS:findPAT done, mOffset=%lld", mOffset);
            mParser->signalDiscontinuity(ATSParser::DISCONTINUITY_TIME
                                         /* isSeek */ );
            while (feedMore() == OK) {  //[qian]the end of file? parse all the TS packet of this file?
                //may be we should add the seek table when 2st parse whole file
#ifdef MTK_AOSP_ENHANCEMENT
                if(((mOffset - mOffsetPAT) > 10000 * kFillPacketSize) && (mParser->getMaxPTS() == 0)) {
                    ALOGD("stop feedmore (no PES) mOffset=%lld  mOffsetPAT=%lld",mOffset,mOffsetPAT);
                    break;                    
                 }
#endif
            }
            mDurationUs = mParser->getMaxPTS();
            if (mDurationUs)
                break;
        }
    }
    //clear data queue
    mParser->signalDiscontinuity(ATSParser::DISCONTINUITY_TIME /* isSeek */ );
    mParser->setDequeueState(true); //
    ALOGD("getMaxPTS->mDurationUs:%lld", mDurationUs);

    return OK;
    //ALOGD("getMaxPTS->mDurationUs:%lld", mDurationUs);
}
uint64_t MPEG2TSExtractor::getDurationUs() {
    return mDurationUs;
}
#endif
#ifdef MTK_AOSP_ENHANCEMENT
void MPEG2TSExtractor::seekTo(int64_t seekTimeUs) {
    Mutex::Autolock autoLock(mLock);

    ALOGE("seekTo:mDurationMs =%lld,seekTimeMs= %lld, mOffset:%lld",
          mDurationUs / 1000, seekTimeUs / 1000, mOffset);
    if (seekTimeUs == 0) {
        mOffset = 0;
        mSeeking = false;
        // clear MaxPTS
        mParser->setDequeueState(false);
        mParser->signalDiscontinuity(ATSParser::DISCONTINUITY_TIME
                                     /* isSeek */ );
        // clear buffer queue
        mParser->setDequeueState(true);
        mParser->signalDiscontinuity(ATSParser::DISCONTINUITY_TIME
                                     /* isSeek */ );
    } else if ((mDurationUs - seekTimeUs) < 10000)  //seek to end
    {
        mOffset = mFileSize;
        mSeeking = false;
        // set ATSParser MaxTimeUs to mDurationUs
        mParser->setDequeueState(false);
        sp<AMessage> maxTimeUsMsg = new AMessage;
        maxTimeUsMsg->setInt64("MaxtimeUs", mDurationUs);
        mParser->signalDiscontinuity(ATSParser::DISCONTINUITY_TIME,
                                     maxTimeUsMsg);
        // clear buffer queue
        mParser->setDequeueState(true);
        mParser->signalDiscontinuity(ATSParser::DISCONTINUITY_TIME
                                     /* isSeek */ );

    } else {
        mParser->signalDiscontinuity(ATSParser::DISCONTINUITY_TIME
                                     /* isSeek */ );

        //[qian] firstly find the rough offset by packet size
        //I thinks we should use the
        //mSeekingOffset=(off64_t)((seekTimeUs*mFileSize/mDuration)/kTSPacketSize)* kTSPacketSize;
        mSeekingOffset = mOffset;

        mSeekTimeUs = seekTimeUs;
        mMinOffset = 0;
        mMaxOffset = mFileSize;
        mMaxcount = 0;
        mParser->setDequeueState(false);    //[qian] will start search mode, not read data mode
        mSeeking = true;
    }
    return;

}
#endif

uint32_t MPEG2TSExtractor::flags() const {
#ifdef MTK_AOSP_ENHANCEMENT
    uint32_t flags = CAN_PAUSE;
    if (!(mDataSource->flags() & DataSource::kIsCachingDataSource)) {   // http streaming do not seek
        flags |= CAN_SEEK_FORWARD | CAN_SEEK_BACKWARD | CAN_SEEK;
    }
	return flags;
#endif
    return CAN_PAUSE;
}

////////////////////////////////////////////////////////////////////////////////
#if defined(MTK_AOSP_ENHANCEMENT) && (defined(SUPPORT_M2TS))
bool SniffMPEG2TS(const sp<DataSource> &source, String8 * mimeType,
                  float *confidence, sp<AMessage> *) {
    bool retb = true;

    for (int i = 0; i < 5; ++i) {
        char header;
        if (source->readAt(kTSPacketSize * i, &header, 1) != 1
            || header != 0x47) {
            retb = false;
            break;
        }
    }
    if (retb) {
        ALOGD("this is ts file\n");
        kFillPacketSize = kTSPacketSize;
    } else {
        retb = true;
        for (int i = 0; i < 5; ++i) {
            char header[5];
            if (source->readAt(kM2TSPacketSize * i, &header, 5) != 5
                || header[4] != 0x47) {
                retb = false;
                return retb;
            }
        }
        if (retb) {
            ALOGD("this is m2ts file\n");
            kFillPacketSize = kM2TSPacketSize;
        }
    }

    *confidence = 0.3f;

    mimeType->setTo(MEDIA_MIMETYPE_CONTAINER_MPEG2TS);

    return true;
}

#else

bool SniffMPEG2TS(
        const sp<DataSource> &source, String8 *mimeType, float *confidence,
        sp<AMessage> *) {
    for (int i = 0; i < 5; ++i) {
        char header;
        if (source->readAt(kTSPacketSize * i, &header, 1) != 1
                || header != 0x47) {
            return false;
        }
    }
#ifdef MTK_AOSP_ENHANCEMENT
    *confidence = 0.3f;
#else
    *confidence = 0.1f;
#endif
    mimeType->setTo(MEDIA_MIMETYPE_CONTAINER_MPEG2TS);

    return true;
}

#endif //#if !defined(ANDROID_DEFAULT_CODE) && (defined(SUPPORT_M2TS))
}  // namespace android
