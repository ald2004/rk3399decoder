#include "rkmppdec.h"


static void rkmpp_release_decoder(void *opaque, uint8_t *data)
{
    RKMPPDecoder *decoder = (RKMPPDecoder *)data;

    if (decoder->mpi) {
        decoder->mpi->reset(decoder->ctx);
        mpp_destroy(decoder->ctx);
        decoder->ctx = NULL;
    }

    if (decoder->frame_group) {
        mpp_buffer_group_put(decoder->frame_group);
        decoder->frame_group = NULL;
    }

    av_buffer_unref(&decoder->frames_ref);
    av_buffer_unref(&decoder->device_ref);

    av_free(decoder);
}

static int rkmpp_write_data(AVCodecContext *avctx, uint8_t *buffer, int size, int64_t pts)
{
    RKMPPDecodeContext *rk_context = (RKMPPDecodeContext*)avctx->priv_data;
    RKMPPDecoder *decoder = (RKMPPDecoder *)rk_context->decoder_ref->data;
    int ret;
    MppPacket packet;

    // create the MPP packet
    ret = mpp_packet_init(&packet, buffer, size);
    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to init MPP packet (code = %d)\n", ret);
        return AVERROR_UNKNOWN;
    }

    mpp_packet_set_pts(packet, pts);

    if (!buffer)
        mpp_packet_set_eos(packet);

    ret = decoder->mpi->decode_put_packet(decoder->ctx, packet);
    if (ret != MPP_OK) {
        if (ret == MPP_ERR_BUFFER_FULL) {
            av_log(avctx, AV_LOG_DEBUG, "Buffer full writing %d bytes to decoder\n", size);
            ret = AVERROR(EAGAIN);
        } else
            ret = AVERROR_UNKNOWN;
    }
    else
        av_log(avctx, AV_LOG_DEBUG, "Wrote %d bytes to decoder\n", size);

    mpp_packet_deinit(&packet);

    return ret;
}

static int rkmpp_close_decoder(AVCodecContext *avctx)
{
    RKMPPDecodeContext *rk_context = (RKMPPDecodeContext*)avctx->priv_data;
    av_buffer_unref(&rk_context->decoder_ref);
    return 0;
}


static int rkmpp_init_decoder(AVCodecContext *avctx)
{
    RKMPPDecodeContext *rk_context = (RKMPPDecodeContext*)avctx->priv_data;
    RKMPPDecoder *decoder = NULL;
    MppCodingType codectype = MPP_VIDEO_CodingUnused;
    int ret;
    RK_S64 paramS64;
    RK_S32 paramS32;

    avctx->pix_fmt = AV_PIX_FMT_DRM_PRIME;

    // create a decoder and a ref to it
    decoder = (RKMPPDecoder*)av_mallocz(sizeof(RKMPPDecoder));
    if (!decoder) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    rk_context->decoder_ref = av_buffer_create((uint8_t *)decoder, sizeof(*decoder), rkmpp_release_decoder,
                                               NULL, AV_BUFFER_FLAG_READONLY);
    if (!rk_context->decoder_ref) {
        av_free(decoder);
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    av_log(avctx, AV_LOG_DEBUG, "Initializing RKMPP decoder.\n");

    codectype = rkmpp_get_codingtype(avctx);
    if (codectype == MPP_VIDEO_CodingUnused) {
        av_log(avctx, AV_LOG_ERROR, "Unknown codec type (%d).\n", avctx->codec_id);
        ret = AVERROR_UNKNOWN;
        goto fail;
    }

    ret = mpp_check_support_format(MPP_CTX_DEC, codectype);
    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Codec type (%d) unsupported by MPP\n", avctx->codec_id);
        ret = AVERROR_UNKNOWN;
        goto fail;
    }

    // Create the MPP context
    ret = mpp_create(&decoder->ctx, &decoder->mpi);
    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to create MPP context (code = %d).\n", ret);
        ret = AVERROR_UNKNOWN;
        goto fail;
    }

    // initialize mpp
    ret = mpp_init(decoder->ctx, MPP_CTX_DEC, codectype);
    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to initialize MPP context (code = %d).\n", ret);
        ret = AVERROR_UNKNOWN;
        goto fail;
    }

    // make decode calls blocking with a timeout
    paramS32 = MPP_POLL_BLOCK;
    ret = decoder->mpi->control(decoder->ctx, MPP_SET_OUTPUT_BLOCK, &paramS32);
    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to set blocking mode on MPI (code = %d).\n", ret);
        ret = AVERROR_UNKNOWN;
        goto fail;
    }

    paramS64 = RECEIVE_FRAME_TIMEOUT;
    ret = decoder->mpi->control(decoder->ctx, MPP_SET_OUTPUT_BLOCK_TIMEOUT, &paramS64);
    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to set block timeout on MPI (code = %d).\n", ret);
        ret = AVERROR_UNKNOWN;
        goto fail;
    }

    ret = mpp_buffer_group_get_internal(&decoder->frame_group, MPP_BUFFER_TYPE_ION);
    if (ret) {
       av_log(avctx, AV_LOG_ERROR, "Failed to retrieve buffer group (code = %d)\n", ret);
       ret = AVERROR_UNKNOWN;
       goto fail;
    }

    ret = decoder->mpi->control(decoder->ctx, MPP_DEC_SET_EXT_BUF_GROUP, decoder->frame_group);
    if (ret) {
        av_log(avctx, AV_LOG_ERROR, "Failed to assign buffer group (code = %d)\n", ret);
        ret = AVERROR_UNKNOWN;
        goto fail;
    }

    ret = mpp_buffer_group_limit_config(decoder->frame_group, 0, FRAMEGROUP_MAX_FRAMES);
    if (ret) {
        av_log(avctx, AV_LOG_ERROR, "Failed to set buffer group limit (code = %d)\n", ret);
        ret = AVERROR_UNKNOWN;
        goto fail;
    }

    decoder->first_packet = 1;

    av_log(avctx, AV_LOG_DEBUG, "RKMPP decoder initialized successfully.\n");

    decoder->device_ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_DRM);
    if (!decoder->device_ref) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    ret = av_hwdevice_ctx_init(decoder->device_ref);
    if (ret < 0)
        goto fail;

    return 0;

fail:
    av_log(avctx, AV_LOG_ERROR, "Failed to initialize RKMPP decoder.\n");
    rkmpp_close_decoder(avctx);
    return ret;
}


int main(){
    // AVFormatContext *pFormatCtx;
    // AVPacket *packet;
    // av_register_all();
    // avformat_network_init();
    // pFormatCtx = avformat_alloc_context();

    // AVCodecContext avctx;
    AVCodec *pCodec = avcodec_find_decoder(AV_CODEC_ID_H264);
    AVCodecContext *pCodecCtx = avcodec_alloc_context3(pCodec);
    // avcodec_parameters_to_context(pCodecCtx, pFormatCtx->streams[videoIndex]->codecpar);

    if (avcodec_open2(pCodecCtx, pCodec, NULL) < 0) {
        printf("Could not open codec.\n");
        return -1;
    }

    RKMPPDecodeContext rkctx;
    pCodecCtx->priv_data = &rkctx;
    av_log_set_level(AV_LOG_DEBUG);
    auto ret=rkmpp_init_decoder(pCodecCtx);
    return 0;
}
