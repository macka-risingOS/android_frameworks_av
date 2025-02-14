/*
 * Copyright (C) 2009 The Android Open Source Project
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
#define LOG_TAG "StagefrightMetadataRetriever"

#include <inttypes.h>

#include <utils/Log.h>
#include <cutils/properties.h>

#include "StagefrightMetadataRetriever.h"
#include "FrameDecoder.h"

#include <datasource/PlayerServiceDataSourceFactory.h>
#include <datasource/PlayerServiceFileSource.h>
#include <media/IMediaHTTPService.h>
#include <media/stagefright/MediaCodecConstants.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/MediaCodecList.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/MediaExtractor.h>
#include <media/stagefright/MediaExtractorFactory.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/Utils.h>
#include <media/CharacterEncodingDetector.h>

namespace android {

StagefrightMetadataRetriever::StagefrightMetadataRetriever()
    : mParsedMetaData(false),
      mAlbumArt(NULL),
      mLastDecodedIndex(-1) {
    ALOGV("StagefrightMetadataRetriever()");
}

StagefrightMetadataRetriever::~StagefrightMetadataRetriever() {
    ALOGV("~StagefrightMetadataRetriever()");
    clearMetadata();
    if (mSource != NULL) {
        mSource->close();
    }
}

status_t StagefrightMetadataRetriever::setDataSource(
        const sp<IMediaHTTPService> &httpService,
        const char *uri,
        const KeyedVector<String8, String8> *headers) {
    ALOGV("setDataSource(%s)", uri);

    clearMetadata();
    mSource = PlayerServiceDataSourceFactory::getInstance()->CreateFromURI(
            httpService, uri, headers);

    if (mSource == NULL) {
        ALOGE("Unable to create data source for '%s'.", uri);
        return UNKNOWN_ERROR;
    }

    mExtractor = MediaExtractorFactory::Create(mSource);

    if (mExtractor == NULL) {
        ALOGE("Unable to instantiate an extractor for '%s'.", uri);

        mSource.clear();

        return UNKNOWN_ERROR;
    }

    return OK;
}

// Warning caller retains ownership of the filedescriptor! Dup it if necessary.
status_t StagefrightMetadataRetriever::setDataSource(
        int fd, int64_t offset, int64_t length) {
    fd = dup(fd);

    ALOGV("setDataSource(%d, %" PRId64 ", %" PRId64 ")", fd, offset, length);

    clearMetadata();
    mSource = new PlayerServiceFileSource(fd, offset, length);

    status_t err;
    if ((err = mSource->initCheck()) != OK) {
        mSource.clear();

        return err;
    }

    mExtractor = MediaExtractorFactory::Create(mSource);

    if (mExtractor == NULL) {
        mSource.clear();

        return UNKNOWN_ERROR;
    }

    return OK;
}

status_t StagefrightMetadataRetriever::setDataSource(
        const sp<DataSource>& source, const char *mime) {
    ALOGV("setDataSource(DataSource)");

    clearMetadata();
    mSource = source;
    mExtractor = MediaExtractorFactory::Create(mSource, mime);

    if (mExtractor == NULL) {
        ALOGE("Failed to instantiate a MediaExtractor.");
        mSource.clear();
        return UNKNOWN_ERROR;
    }

    return OK;
}

sp<IMemory> StagefrightMetadataRetriever::getImageAtIndex(
        int index, int colorFormat, bool metaOnly, bool thumbnail) {
    ALOGV("getImageAtIndex: index(%d) colorFormat(%d) metaOnly(%d) thumbnail(%d)",
            index, colorFormat, metaOnly, thumbnail);

    return getImageInternal(index, colorFormat, metaOnly, thumbnail, NULL);
}

sp<IMemory> StagefrightMetadataRetriever::getImageRectAtIndex(
        int index, int colorFormat, int left, int top, int right, int bottom) {
    ALOGV("getImageRectAtIndex: index(%d) colorFormat(%d) rect {%d, %d, %d, %d}",
            index, colorFormat, left, top, right, bottom);

    FrameRect rect = {left, top, right, bottom};

    if (mDecoder != NULL && index == mLastDecodedIndex) {
        return mDecoder->extractFrame(&rect);
    }

    return getImageInternal(
            index, colorFormat, false /*metaOnly*/, false /*thumbnail*/, &rect);
}

sp<IMemory> StagefrightMetadataRetriever::getImageInternal(
        int index, int colorFormat, bool metaOnly, bool thumbnail, FrameRect* rect) {
    mDecoder.clear();
    mLastDecodedIndex = -1;

    if (mExtractor.get() == NULL) {
        ALOGE("no extractor.");
        return NULL;
    }

    size_t n = mExtractor->countTracks();
    size_t i;
    int imageCount = 0;

    for (i = 0; i < n; ++i) {
        sp<MetaData> meta = mExtractor->getTrackMetaData(i);
        if (!meta) {
            continue;
        }
        ALOGV("getting track %zu of %zu, meta=%s", i, n, meta->toString().c_str());

        const char *mime;
        if (meta->findCString(kKeyMIMEType, &mime) && !strncasecmp(mime, "image/", 6)) {
            int32_t isPrimary;
            if ((index < 0 && meta->findInt32(
                    kKeyTrackIsDefault, &isPrimary) && isPrimary)
                    || (index == imageCount++)) {
                break;
            }
        }
    }

    if (i == n) {
        ALOGE("image track not found.");
        return NULL;
    }

    sp<MetaData> trackMeta = mExtractor->getTrackMetaData(i);
    if (!trackMeta) {
        return NULL;
    }

    const char *mime;
    bool isHeif = false;
    if (!trackMeta->findCString(kKeyMIMEType, &mime)) {
        ALOGE("image track has no mime type");
        return NULL;
    }
    ALOGV("extracting from %s track", mime);
    if (!strcasecmp(mime, MEDIA_MIMETYPE_IMAGE_ANDROID_HEIC)) {
        mime = MEDIA_MIMETYPE_VIDEO_HEVC;
        trackMeta = new MetaData(*trackMeta);
        trackMeta->setCString(kKeyMIMEType, mime);
        isHeif = true;
    } else if (!strcasecmp(mime, MEDIA_MIMETYPE_IMAGE_AVIF)) {
        mime = MEDIA_MIMETYPE_VIDEO_AV1;
        trackMeta = new MetaData(*trackMeta);
        trackMeta->setCString(kKeyMIMEType, mime);
        isHeif = true;
    }

    sp<AMessage> format = new AMessage;
    status_t err = convertMetaDataToMessage(trackMeta, &format);
    if (err != OK) {
        ALOGE("getImageInternal: convertMetaDataToMessage() failed, unable to extract image");
        return NULL;
    }

    uint32_t bitDepth = 8;
    if (!strcasecmp(mime, MEDIA_MIMETYPE_VIDEO_HEVC)) {
        int32_t profile;
        if (format->findInt32("profile", &profile)) {
            if (HEVCProfileMain10 == profile || HEVCProfileMain10HDR10 == profile ||
                    HEVCProfileMain10HDR10Plus == profile) {
                  bitDepth = 10;
            }
        }
    } else if (!strcasecmp(mime, MEDIA_MIMETYPE_VIDEO_AV1)) {
        int32_t profile;
        if (format->findInt32("profile", &profile)) {
            if (AV1ProfileMain10 == profile || AV1ProfileMain10HDR10 == profile ||
                    AV1ProfileMain10HDR10Plus == profile) {
                  bitDepth = 10;
            }
        }
    }

    if (bitDepth == 10) {
        /* b/258355840
         * 10bit thumbnail is supported since android T launching devices
         * some components may not support COLOR_FormatYUVP010
         */
        bool hardwareCodecSupportsP010 = true;
        const sp<IMediaCodecList> list = MediaCodecList::getInstance();
        if (list != nullptr) {
            size_t index = 0;
            for (;;) {
                ssize_t matchIndex =
                    list->findCodecByType(mime, false /* encoder */, index);

                if (matchIndex < 0) {
                    break;
                }
                index = matchIndex + 1;

                const sp<MediaCodecInfo> info = list->getCodecInfo(matchIndex);
                if ((info == nullptr) ||
                    /* regardless of supportability of s/w codec */
                    (MediaCodecList::isSoftwareCodec(AString(info->getCodecName())))) {
                    continue;
                }

                const sp<MediaCodecInfo::Capabilities> caps =
                        info->getCapabilitiesFor(mime);
                if (caps == nullptr) {
                    continue;
                }

                hardwareCodecSupportsP010 = false;
                Vector<uint32_t> colors;
                caps->getSupportedColorFormats(&colors);
                for (uint32_t color : colors) {
                    if (color == COLOR_FormatYUVP010) {
                        hardwareCodecSupportsP010 = true;
                        break;
                    }
                }

                if (!hardwareCodecSupportsP010) {
                    /* 10bit could be handled when all codecs support P010 */
                    break;
                }
            }
        }

        if (!hardwareCodecSupportsP010) {
            ALOGD("COLOR_FormatYUVP010 is not supported. restrict to 8bit");
            bitDepth = 8;
        }
    }

    if (metaOnly) {
        return FrameDecoder::getMetadataOnly(trackMeta, colorFormat, thumbnail, bitDepth);
    }

    sp<IMediaSource> source = mExtractor->getTrack(i);

    if (source.get() == NULL) {
        ALOGE("unable to instantiate image track.");
        return NULL;
    }

    bool preferhw = property_get_bool(
            "media.stagefright.thumbnail.prefer_hw_codecs", false);
    uint32_t flags = preferhw ? 0 : MediaCodecList::kPreferSoftwareCodecs;
    Vector<AString> matchingCodecs;

    // If decoding thumbnail check decoder supports thumbnail dimensions instead
    int32_t thumbHeight, thumbWidth;
    if (thumbnail && format != NULL
            && trackMeta->findInt32(kKeyThumbnailHeight, &thumbHeight)
            && trackMeta->findInt32(kKeyThumbnailWidth, &thumbWidth)) {
        format->setInt32("height", thumbHeight);
        format->setInt32("width", thumbWidth);
    }

    // If decoding tiled HEIF check decoder supports tile dimensions instead
    if (!thumbnail && isHeif && format != NULL) {
        int32_t tileWidth, tileHeight;
        if (trackMeta->findInt32(kKeyTileWidth, &tileWidth) && tileWidth > 0
                && trackMeta->findInt32(kKeyTileHeight, &tileHeight) && tileHeight > 0) {
            format->setInt32("height", tileHeight);
            format->setInt32("width", tileWidth);
        }
    }

    MediaCodecList::findMatchingCodecs(
            mime,
            false, /* encoder */
            flags,
            format,
            &matchingCodecs);

    for (size_t i = 0; i < matchingCodecs.size(); ++i) {
        const AString &componentName = matchingCodecs[i];
        sp<MediaImageDecoder> decoder = new MediaImageDecoder(componentName, trackMeta, source);
        int64_t frameTimeUs = thumbnail ? -1 : 0;
        if (decoder->init(frameTimeUs, 0 /*option*/, colorFormat) == OK) {
            sp<IMemory> frame = decoder->extractFrame(rect);

            if (frame != NULL) {
                if (rect != NULL) {
                    // keep the decoder if slice decoding
                    mDecoder = decoder;
                    mLastDecodedIndex = index;
                }
                return frame;
            }
        }
        ALOGV("%s failed to extract thumbnail, trying next decoder.", componentName.c_str());
    }

    ALOGE("all codecs failed to extract frame.");
    return NULL;
}

sp<IMemory> StagefrightMetadataRetriever::getFrameAtTime(
        int64_t timeUs, int option, int colorFormat, bool metaOnly) {
    ALOGV("getFrameAtTime: %" PRId64 " us option: %d colorFormat: %d, metaOnly: %d",
            timeUs, option, colorFormat, metaOnly);

    return getFrameInternal(timeUs, option, colorFormat, metaOnly);
}

sp<IMemory> StagefrightMetadataRetriever::getFrameAtIndex(
        int frameIndex, int colorFormat, bool metaOnly) {
    ALOGV("getFrameAtIndex: frameIndex %d, colorFormat: %d, metaOnly: %d",
            frameIndex, colorFormat, metaOnly);
    if (mDecoder != NULL && frameIndex == mLastDecodedIndex + 1) {
        sp<IMemory> frame = mDecoder->extractFrame();
        if (frame != nullptr) {
            mLastDecodedIndex = frameIndex;
        }
        return frame;
    }

    return getFrameInternal(frameIndex,
            MediaSource::ReadOptions::SEEK_FRAME_INDEX, colorFormat, metaOnly);
}

sp<IMemory> StagefrightMetadataRetriever::getFrameInternal(
        int64_t timeUs, int option, int colorFormat, bool metaOnly) {
    mDecoder.clear();
    mLastDecodedIndex = -1;

    if (mExtractor.get() == NULL) {
        ALOGE("no extractor.");
        return NULL;
    }

    sp<MetaData> fileMeta = mExtractor->getMetaData();

    if (fileMeta == NULL) {
        ALOGE("extractor doesn't publish metadata, failed to initialize?");
        return NULL;
    }

    size_t n = mExtractor->countTracks();
    size_t i;
    for (i = 0; i < n; ++i) {
        sp<MetaData> meta = mExtractor->getTrackMetaData(i);
        if (!meta) {
            continue;
        }

        const char *mime;
        if (meta->findCString(kKeyMIMEType, &mime) && !strncasecmp(mime, "video/", 6)) {
            break;
        }
    }

    if (i == n) {
        ALOGE("no video track found.");
        return NULL;
    }

    sp<MetaData> trackMeta = mExtractor->getTrackMetaData(
            i, MediaExtractor::kIncludeExtensiveMetaData);
    if (!trackMeta) {
        return NULL;
    }

    if (metaOnly) {
        return FrameDecoder::getMetadataOnly(trackMeta, colorFormat);
    }

    sp<IMediaSource> source = mExtractor->getTrack(i);

    if (source.get() == NULL) {
        ALOGV("unable to instantiate video track.");
        return NULL;
    }

    const void *data;
    uint32_t type;
    size_t dataSize;
    if (fileMeta->findData(kKeyAlbumArt, &type, &data, &dataSize)
            && mAlbumArt == NULL) {
        mAlbumArt = MediaAlbumArt::fromData(dataSize, data);
    }

    const char *mime;
    if (!trackMeta->findCString(kKeyMIMEType, &mime)) {
        ALOGE("video track has no mime information.");
        return NULL;
    }

    bool preferhw = property_get_bool(
            "media.stagefright.thumbnail.prefer_hw_codecs", false);
    uint32_t flags = preferhw ? 0 : MediaCodecList::kPreferSoftwareCodecs;
    sp<AMessage> format = new AMessage;
    status_t err = convertMetaDataToMessage(trackMeta, &format);
    if (err != OK) {
        ALOGE("getFrameInternal: convertMetaDataToMessage() failed, unable to extract frame");
        return NULL;
    }

    Vector<AString> matchingCodecs;
    MediaCodecList::findMatchingCodecs(
            mime,
            false, /* encoder */
            flags,
            format,
            &matchingCodecs);

    for (size_t i = 0; i < matchingCodecs.size(); ++i) {
        const AString &componentName = matchingCodecs[i];
        sp<VideoFrameDecoder> decoder = new VideoFrameDecoder(componentName, trackMeta, source);
        if (decoder->init(timeUs, option, colorFormat) == OK) {
            sp<IMemory> frame = decoder->extractFrame();
            if (frame != nullptr) {
                // keep the decoder if seeking by frame index
                if (option == MediaSource::ReadOptions::SEEK_FRAME_INDEX) {
                    mDecoder = decoder;
                    mLastDecodedIndex = timeUs;
                }
                return frame;
            }
        }
        ALOGV("%s failed to extract frame, trying next decoder.", componentName.c_str());
    }

    ALOGE("all codecs failed to extract frame.");
    return NULL;
}

MediaAlbumArt *StagefrightMetadataRetriever::extractAlbumArt() {
    ALOGV("extractAlbumArt (extractor: %s)", mExtractor.get() != NULL ? "YES" : "NO");

    if (mExtractor == NULL) {
        return NULL;
    }

    if (!mParsedMetaData) {
        parseMetaData();

        mParsedMetaData = true;
    }

    if (mAlbumArt) {
        return mAlbumArt->clone();
    }

    return NULL;
}

const char *StagefrightMetadataRetriever::extractMetadata(int keyCode) {
    if (mExtractor == NULL) {
        return NULL;
    }

    if (!mParsedMetaData) {
        parseMetaData();

        mParsedMetaData = true;
    }

    ssize_t index = mMetaData.indexOfKey(keyCode);

    if (index < 0) {
        return NULL;
    }

    return mMetaData.valueAt(index).string();
}

void StagefrightMetadataRetriever::parseColorAspects(const sp<MetaData>& meta) {
    sp<AMessage> format = new AMessage();
    if (convertMetaDataToMessage(meta, &format) != OK) {
        return;
    }

    int32_t standard, transfer, range;
    if (format->findInt32("color-standard", &standard)
            && format->findInt32("color-transfer", &transfer)
            && format->findInt32("color-range", &range)) {
        ALOGV("found color aspects : standard=%d, transfer=%d, range=%d",
                standard, transfer, range);

        mMetaData.add(METADATA_KEY_COLOR_STANDARD, String8::format("%d", standard));
        mMetaData.add(METADATA_KEY_COLOR_TRANSFER, String8::format("%d", transfer));
        mMetaData.add(METADATA_KEY_COLOR_RANGE, String8::format("%d", range));
    }
}

void StagefrightMetadataRetriever::parseMetaData() {
    sp<MetaData> meta = mExtractor->getMetaData();

    if (meta == NULL) {
        ALOGV("extractor doesn't publish metadata, failed to initialize?");
        return;
    }

    struct Map {
        int from;
        int to;
        const char *name;
    };
    static const Map kMap[] = {
        { kKeyMIMEType, METADATA_KEY_MIMETYPE, NULL },
        { kKeyCDTrackNumber, METADATA_KEY_CD_TRACK_NUMBER, "tracknumber" },
        { kKeyDiscNumber, METADATA_KEY_DISC_NUMBER, "discnumber" },
        { kKeyAlbum, METADATA_KEY_ALBUM, "album" },
        { kKeyArtist, METADATA_KEY_ARTIST, "artist" },
        { kKeyAlbumArtist, METADATA_KEY_ALBUMARTIST, "albumartist" },
        { kKeyAuthor, METADATA_KEY_AUTHOR, NULL },
        { kKeyComposer, METADATA_KEY_COMPOSER, "composer" },
        { kKeyDate, METADATA_KEY_DATE, NULL },
        { kKeyGenre, METADATA_KEY_GENRE, "genre" },
        { kKeyTitle, METADATA_KEY_TITLE, "title" },
        { kKeyYear, METADATA_KEY_YEAR, "year" },
        { kKeyWriter, METADATA_KEY_WRITER, "writer" },
        { kKeyCompilation, METADATA_KEY_COMPILATION, "compilation" },
        { kKeyLocation, METADATA_KEY_LOCATION, NULL },
    };

    static const size_t kNumMapEntries = sizeof(kMap) / sizeof(kMap[0]);

    CharacterEncodingDetector *detector = new CharacterEncodingDetector();

    for (size_t i = 0; i < kNumMapEntries; ++i) {
        const char *value;
        if (meta->findCString(kMap[i].from, &value)) {
            if (kMap[i].name) {
                // add to charset detector
                detector->addTag(kMap[i].name, value);
            } else {
                // directly add to output list
                mMetaData.add(kMap[i].to, String8(value));
            }
        }
    }

    detector->detectAndConvert();
    int size = detector->size();
    if (size) {
        for (int i = 0; i < size; i++) {
            const char *name;
            const char *value;
            detector->getTag(i, &name, &value);
            for (size_t j = 0; j < kNumMapEntries; ++j) {
                if (kMap[j].name && !strcmp(kMap[j].name, name)) {
                    mMetaData.add(kMap[j].to, String8(value));
                }
            }
        }
    }
    delete detector;

    const void *data;
    uint32_t type;
    size_t dataSize;
    if (meta->findData(kKeyAlbumArt, &type, &data, &dataSize)
            && mAlbumArt == NULL) {
        mAlbumArt = MediaAlbumArt::fromData(dataSize, data);
    }

    size_t numTracks = mExtractor->countTracks();

    char tmp[32];
    sprintf(tmp, "%zu", numTracks);

    mMetaData.add(METADATA_KEY_NUM_TRACKS, String8(tmp));

    float captureFps;
    if (meta->findFloat(kKeyCaptureFramerate, &captureFps)) {
        sprintf(tmp, "%f", captureFps);
        mMetaData.add(METADATA_KEY_CAPTURE_FRAMERATE, String8(tmp));
    }

    int64_t exifOffset, exifSize;
    if (meta->findInt64(kKeyExifOffset, &exifOffset)
     && meta->findInt64(kKeyExifSize, &exifSize)) {
        sprintf(tmp, "%lld", (long long)exifOffset);
        mMetaData.add(METADATA_KEY_EXIF_OFFSET, String8(tmp));
        sprintf(tmp, "%lld", (long long)exifSize);
        mMetaData.add(METADATA_KEY_EXIF_LENGTH, String8(tmp));
    }

    int64_t xmpOffset, xmpSize;
    if (meta->findInt64(kKeyXmpOffset, &xmpOffset)
     && meta->findInt64(kKeyXmpSize, &xmpSize)) {
        sprintf(tmp, "%lld", (long long)xmpOffset);
        mMetaData.add(METADATA_KEY_XMP_OFFSET, String8(tmp));
        sprintf(tmp, "%lld", (long long)xmpSize);
        mMetaData.add(METADATA_KEY_XMP_LENGTH, String8(tmp));
    }

    bool hasAudio = false;
    bool hasVideo = false;
    int32_t videoWidth = -1;
    int32_t videoHeight = -1;
    int32_t videoFrameCount = 0;
    int32_t audioBitrate = -1;
    int32_t rotationAngle = -1;
    int32_t imageCount = 0;
    int32_t imagePrimary = -1;
    int32_t imageWidth = -1;
    int32_t imageHeight = -1;
    int32_t imageRotation = -1;

    // The overall duration is the duration of the longest track.
    int64_t maxDurationUs = 0;
    String8 timedTextLang, videoMime;
    for (size_t i = 0; i < numTracks; ++i) {
        sp<MetaData> trackMeta = mExtractor->getTrackMetaData(i);
        if (!trackMeta) {
            continue;
        }

        int64_t durationUs;
        if (trackMeta->findInt64(kKeyDuration, &durationUs)) {
            if (durationUs > maxDurationUs) {
                maxDurationUs = durationUs;
            }
        }

        const char *mime;
        if (trackMeta->findCString(kKeyMIMEType, &mime)) {
            if (!hasAudio && !strncasecmp("audio/", mime, 6)) {
                hasAudio = true;

                if (!trackMeta->findInt32(kKeyBitRate, &audioBitrate)) {
                    audioBitrate = -1;
                }

                int32_t bitsPerSample = -1;
                int32_t sampleRate = -1;
                trackMeta->findInt32(kKeyBitsPerSample, &bitsPerSample);
                trackMeta->findInt32(kKeySampleRate, &sampleRate);
                if (bitsPerSample >= 0) {
                    sprintf(tmp, "%d", bitsPerSample);
                    mMetaData.add(METADATA_KEY_BITS_PER_SAMPLE, String8(tmp));
                }
                if (sampleRate >= 0) {
                    sprintf(tmp, "%d", sampleRate);
                    mMetaData.add(METADATA_KEY_SAMPLERATE, String8(tmp));
                }
            } else if (!hasVideo && !strncasecmp("video/", mime, 6)) {
                if (!trackMeta->findInt32(kKeyRotation, &rotationAngle)) {
                    rotationAngle = 0;
                }
                if (!trackMeta->findInt32(kKeyFrameCount, &videoFrameCount)) {
                    videoFrameCount = 0;
                }
                if (trackMeta->findInt32(kKeyWidth, &videoWidth)
                    && trackMeta->findInt32(kKeyHeight, &videoHeight)) {
                    hasVideo = true;
                    videoMime = String8(mime);
                    parseColorAspects(trackMeta);
                } else {
                    ALOGE("video track ignored for missing dimensions");
                }
            } else if (!strncasecmp("image/", mime, 6)) {
                int32_t isPrimary;
                if (trackMeta->findInt32(
                        kKeyTrackIsDefault, &isPrimary) && isPrimary) {
                    if (!trackMeta->findInt32(kKeyRotation, &imageRotation)) {
                        imageRotation = 0;
                    }
                    if (trackMeta->findInt32(kKeyWidth, &imageWidth)
                        && trackMeta->findInt32(kKeyHeight, &imageHeight)) {
                        imagePrimary = imageCount;
                    } else {
                        ALOGE("primary image track ignored for missing dimensions");
                    }
                }
                imageCount++;
            } else if (!strcasecmp(mime, MEDIA_MIMETYPE_TEXT_3GPP)) {
                const char *lang;
                if (trackMeta->findCString(kKeyMediaLanguage, &lang)) {
                    timedTextLang.append(String8(lang));
                    timedTextLang.append(String8(":"));
                } else {
                    ALOGE("No language found for timed text");
                }
            }
        }
    }

    // To save the language codes for all timed text tracks
    // If multiple text tracks present, the format will look
    // like "eng:chi"
    if (!timedTextLang.isEmpty()) {
        mMetaData.add(METADATA_KEY_TIMED_TEXT_LANGUAGES, timedTextLang);
    }

    // The duration value is a string representing the duration in ms.
    sprintf(tmp, "%" PRId64,
           (maxDurationUs > (INT64_MAX - 500) ? INT64_MAX : (maxDurationUs + 500)) / 1000);
    mMetaData.add(METADATA_KEY_DURATION, String8(tmp));

    if (hasAudio) {
        mMetaData.add(METADATA_KEY_HAS_AUDIO, String8("yes"));
    }

    if (hasVideo) {
        mMetaData.add(METADATA_KEY_HAS_VIDEO, String8("yes"));

        CHECK(videoWidth >= 0);
        sprintf(tmp, "%d", videoWidth);
        mMetaData.add(METADATA_KEY_VIDEO_WIDTH, String8(tmp));

        CHECK(videoHeight >= 0);
        sprintf(tmp, "%d", videoHeight);
        mMetaData.add(METADATA_KEY_VIDEO_HEIGHT, String8(tmp));

        sprintf(tmp, "%d", rotationAngle);
        mMetaData.add(METADATA_KEY_VIDEO_ROTATION, String8(tmp));

        mMetaData.add(METADATA_KEY_VIDEO_CODEC_MIME_TYPE, videoMime);

        if (videoFrameCount > 0) {
            sprintf(tmp, "%d", videoFrameCount);
            mMetaData.add(METADATA_KEY_VIDEO_FRAME_COUNT, String8(tmp));
        }
    }

    // only if we have a primary image
    if (imageCount > 0 && imagePrimary >= 0) {
        mMetaData.add(METADATA_KEY_HAS_IMAGE, String8("yes"));

        sprintf(tmp, "%d", imageCount);
        mMetaData.add(METADATA_KEY_IMAGE_COUNT, String8(tmp));

        sprintf(tmp, "%d", imagePrimary);
        mMetaData.add(METADATA_KEY_IMAGE_PRIMARY, String8(tmp));

        CHECK(imageWidth >= 0);
        sprintf(tmp, "%d", imageWidth);
        mMetaData.add(METADATA_KEY_IMAGE_WIDTH, String8(tmp));

        CHECK(imageHeight >= 0);
        sprintf(tmp, "%d", imageHeight);
        mMetaData.add(METADATA_KEY_IMAGE_HEIGHT, String8(tmp));

        sprintf(tmp, "%d", imageRotation);
        mMetaData.add(METADATA_KEY_IMAGE_ROTATION, String8(tmp));
    }

    if (numTracks == 1 && hasAudio && audioBitrate >= 0) {
        sprintf(tmp, "%d", audioBitrate);
        mMetaData.add(METADATA_KEY_BITRATE, String8(tmp));
    } else {
        off64_t sourceSize;
        if (mSource != NULL && mSource->getSize(&sourceSize) == OK) {
            int64_t avgBitRate = (int64_t)(sourceSize * 8E6 / maxDurationUs);

            sprintf(tmp, "%" PRId64, avgBitRate);
            mMetaData.add(METADATA_KEY_BITRATE, String8(tmp));
        }
    }

    if (numTracks == 1) {
        const char *fileMIME;

        if (meta->findCString(kKeyMIMEType, &fileMIME) &&
                !strcasecmp(fileMIME, "video/x-matroska")) {
            sp<MetaData> trackMeta = mExtractor->getTrackMetaData(0);
            const char *trackMIME;
            if (trackMeta != nullptr
                && trackMeta->findCString(kKeyMIMEType, &trackMIME)
                && !strncasecmp("audio/", trackMIME, 6)) {
                // The matroska file only contains a single audio track,
                // rewrite its mime type.
                mMetaData.add(
                        METADATA_KEY_MIMETYPE, String8("audio/x-matroska"));
            }
        }
    }
}

void StagefrightMetadataRetriever::clearMetadata() {
    mParsedMetaData = false;
    mMetaData.clear();
    delete mAlbumArt;
    mAlbumArt = NULL;
}

}  // namespace android
