
/**
*   基于ffmpeg，在linux平台下，获取摄像头数据并通过fitler（作为适配器）后放入编码器（MPEG1VIDEO），
* 再通过封装的写入文件方式（以此使每一帧带有帧头，以标准MPEG1数据流）发送到httpURL中去。
* 同时通过SDL把数据流显示在屏幕上。
*
*    This file is based on FFMpeg in Linux.
* It gets data stream from camera in advice(with video4linux2)
* go through by AVFilter to fit the size of each frame 
* and encode it with MPEG1VIDEO encoder,and write it to a httpURL.
* Meanwhile show the camera data stream on screen .
* 
*
*   $author = gmliuruiqiang@126.com
*   $version = 2.0
*
* ������ʵ����linuxƽ̨�£���ȡ����ͷ�����������롢����Ϊmpeg1VIDEO��ת���ÿһ֡��socket���ͳ�ȥ��
*/



#include <stdio.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/shm.h>
#include <signal.h>



#define __STDC_CONSTANT_MACROS

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavdevice/avdevice.h>
#include <SDL/SDL.h>
#include "libavfilter/buffersink.h"
#include "libavfilter/buffersrc.h"
#include "libavutil/avutil.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavfilter/avfiltergraph.h"
};


#define SFM_REFRESH_EVENT  (SDL_USEREVENT + 1)
#define SFM_BREAK_EVENT  (SDL_USEREVENT + 2)  

int thread_exit = 0;
static int videoindex = -1;                //video index in stream.
static int nFrameCount = 0;
static AVFormatContext *ifmtCtx;
static AVFormatContext *ofmtCtx;

AVCodecID en_codec_id = AV_CODEC_ID_MPEG1VIDEO;    // encoder 
//AVCodecID en_codec_id = AV_CODEC_ID_H264;


typedef struct FilteringContext 
{
	AVFilterContext* buffersink_ctx;
	AVFilterContext* buffersrc_ctx;
	AVFilterGraph* filter_graph;
} FilteringContext;

static FilteringContext *filter_ctx;
SDL_Overlay *bmp;

static int initFilter(AVCodecContext *dec_ctx,AVCodecContext *enc_ctx)
{
	filter_ctx = (FilteringContext*)av_malloc(sizeof(*filter_ctx));
	if (!filter_ctx)
		return AVERROR(ENOMEM);
	filter_ctx->buffersrc_ctx = NULL;
	filter_ctx->buffersink_ctx = NULL;
	filter_ctx->filter_graph = NULL;

	char args[512];
	int ret = 0;
	AVFilter* buffersrc = NULL;
	AVFilter* buffersink = NULL;
	AVFilterContext* buffersrc_ctx = NULL;
	AVFilterContext* buffersink_ctx = NULL;
	AVFilterInOut* outputs = avfilter_inout_alloc();
	AVFilterInOut* inputs = avfilter_inout_alloc();
	AVFilterGraph* filter_graph = avfilter_graph_alloc();
	if (!outputs || !inputs || !filter_graph)
	{
		ret = AVERROR(ENOMEM);
		goto end;                            //free input/output
	}
	buffersrc = avfilter_get_by_name("buffer");                    //��ȡҪʹ�õĹ�����
	buffersink = avfilter_get_by_name("buffersink");
	if (!buffersrc || !buffersink)
	{
		av_log(NULL, AV_LOG_ERROR, "filtering source or sink element not found\n");
		ret = AVERROR_UNKNOWN;
		goto end;
	}
	ret = snprintf(args, sizeof(args), "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
		dec_ctx->width, dec_ctx->height, dec_ctx->pix_fmt, dec_ctx->time_base.num, dec_ctx->time_base.den,
		dec_ctx->sample_aspect_ratio.num, dec_ctx->sample_aspect_ratio.den);
	if (ret == sizeof(args) || ret < 0)
	{
		av_log(NULL, AV_LOG_ERROR, "args is too short for format.");                 //check is args illegal or not
		ret = AVERROR_UNKNOWN;
		goto end;
	}
	ret = avfilter_graph_create_filter(&buffersrc_ctx, buffersrc, "in", args, NULL, filter_graph);   //����������������
	if (ret < 0)
	{
		av_log(NULL, AV_LOG_ERROR, "Cannotcreate buffer source\n");
		goto end;
	}
	ret = avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out", NULL, NULL, filter_graph);
	if (ret < 0)
	{
		av_log(NULL, AV_LOG_ERROR, "Cannotcreate buffer sink\n");
		goto end;
	}
	ret = av_opt_set_bin(buffersink_ctx,"pix_fmts",(uint8_t*)&enc_ctx->pix_fmt,sizeof(enc_ctx->pix_fmt),AV_OPT_SEARCH_CHILDREN);
	if(ret<0)
	{
		av_log(NULL,AV_LOG_ERROR,"set output pixel format wrong.");
		goto end;
	}
	outputs->name = av_strdup("in");
	outputs->filter_ctx = buffersrc_ctx;
	outputs->pad_idx = 0;
	outputs->next = NULL;

	inputs->name = av_strdup("out");
	inputs->filter_ctx = buffersink_ctx;
	inputs->pad_idx = 0;
	inputs->next = NULL;
	if (!outputs->name || !inputs->name) {
		ret = AVERROR(ENOMEM);
		goto end;
	}
	if ((ret = avfilter_graph_parse_ptr(filter_graph, "null", &inputs, &outputs, NULL)) < 0)
		goto end;
	if ((ret = avfilter_graph_config(filter_graph, NULL))< 0)
		goto end;
	/* Fill FilteringContext */
	filter_ctx->buffersrc_ctx = buffersrc_ctx;
	filter_ctx->buffersink_ctx = buffersink_ctx;
	filter_ctx->filter_graph = filter_graph;
end:
	avfilter_inout_free(&inputs);
	avfilter_inout_free(&outputs);
	return ret;
}
static int encodeFrame(AVCodecContext *enc_ctx, AVFrame *filter_frame)
{
	//img_convert_ctx = sws_getContext(pDeCodecCtx->width, pDeCodecCtx->height, pDeCodecCtx->pix_fmt, 640, 480, AV_PIX_FMT_YUV420P, SWS_BICUBIC, NULL, NULL, NULL);
	int ret;
	int gotFrame;
	AVFrame *dstFrame;
	dstFrame = av_frame_alloc();
	dstFrame->linesize[0] = bmp->pitches[0];
	dstFrame->linesize[1] = bmp->pitches[2];
	dstFrame->linesize[2] = bmp->pitches[1];
	dstFrame->data[0] = bmp->pixels[0];
	dstFrame->data[1] = bmp->pixels[2];
	dstFrame->data[2] = bmp->pixels[1];
	//sws_scale(img_convert_ctx,(const unsigned char* const*)filter_frame,filter_frame->linesize,0,480,dstFrame->data,dstFrame->linesize);
	AVPacket encPkt;
	encPkt.data = NULL;
	encPkt.size = 0;
	av_init_packet(&encPkt);
	//dstFrame->pict_type = AV_PICTURE_TYPE_NONE;
	av_opt_set(enc_ctx->priv_data,"tune","zerolatency",0);
	ret = avcodec_encode_video2(enc_ctx, &encPkt, dstFrame, &gotFrame);
	av_frame_free(&dstFrame);
	if (ret < 0)
	{
		return ret;
	}
	if (gotFrame)
	{
		encPkt.stream_index = videoindex;
		encPkt.dts = av_rescale_q_rnd(encPkt.dts, enc_ctx->time_base, ofmtCtx->streams[videoindex]->time_base, (AVRounding)(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
		encPkt.pts = av_rescale_q_rnd(encPkt.pts, enc_ctx->time_base, ofmtCtx->streams[videoindex]->time_base, (AVRounding)(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
		encPkt.duration = av_rescale_q(encPkt.duration, enc_ctx->time_base, ofmtCtx->streams[videoindex]->time_base);
		av_interleaved_write_frame(ofmtCtx,&encPkt);
		nFrameCount++;
	}
	av_free_packet(&encPkt);
	return ret;
}

static int filterOutputFrame(AVFrame *frame, AVCodecContext *enc_ctx)
{

	AVFrame *filterFrame;
	//av_log(NULL, AV_LOG_INFO, "Pushing decoded frame to filters\n");
	int ret = av_buffersrc_add_frame_flags(filter_ctx->buffersrc_ctx, frame, 0);
	if (ret < 0)
	{
		av_log(NULL, AV_LOG_ERROR, "Error while feeding the filtergraph\n");
		return ret;
	}
	while (1)
	{
		filterFrame = av_frame_alloc();
		if (!filterFrame)
	
		{
			ret = AVERROR(ENOMEM);
			break;
		}
		ret = av_buffersink_get_frame(filter_ctx->buffersink_ctx, filterFrame);
		if (ret < 0)
		{
			/* if nomore frames for output - returns AVERROR(EAGAIN)
			* if flushed and no more frames for output - returns AVERROR_EOF
			* rewrite retcode to 0 to show it as normal procedure completion
			*/
			if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
				ret = 0;
			av_frame_free(&filterFrame);
			break;
		}
		ret = encodeFrame(enc_ctx, filterFrame);
		av_frame_free(&filterFrame);

		if (ret < 0)
			break;
	}
	return ret;
}
static int flush_encoder(unsigned int stream_index)
{
	//int i = 0;
	//int ret = 0;
	//for (int got_output = 1; got_output; i++) {
	//	ret = avcodec_encode_video2(pCodecCtx, &pkt, NULL, &got_output);
	//	if (ret < 0) {
	//		printf("Error encoding frame\n");
	//		return -1;
	//	}
	//	if (got_output) {
	//		printf("Flush Encoder: Succeed to encode 1 frame!\tsize:%5d\n", pkt.size);
	//		fwrite(pkt.data, 1, pkt.size, fp_out);
	//		av_free_packet(&pkt);
	//	}
	//}
}

int sfp_refresh_thread(void *opaque)
{
	thread_exit = 0;
	while (!thread_exit) 
	{
		SDL_Event event;
		event.type = SFM_REFRESH_EVENT;
		SDL_PushEvent(&event);
		SDL_Delay(40);
	}
	thread_exit = 0;
	//Break
	SDL_Event event;
	event.type = SFM_BREAK_EVENT;
	SDL_PushEvent(&event);

	return 0;
}
int main(int argc, char* argv[])
{
	int ret;
	AVPacket tranPkt;
	enum AVMediaType type;
	AVCodecContext	*pDeCodecCtx,*pEnCodecCtx;
	AVCodec			*pDeCodec, *pEnCodec;
	AVFrame	*pFrame, *pFrameYUV;
	AVStream *out_stream;
	AVStream *out_stream2;
	SDL_Thread *video_tid;
	int screen_w = 0, screen_h = 0;
	int got_frame;
	av_register_all();
	avfilter_register_all();
	avformat_network_init();
	ifmtCtx = avformat_alloc_context();
	avdevice_register_all();
	//***************************************************************write in file ******************************************
	//const char* out_filename = "http://192.168.220.239:8081/123/640/480";
	const char* out_filename = "http://192.168.220.210:8080";
	//const char* out_filename = "./video.mp4";
	
	//*********************************************************??????????packet alloc****************************************//
	/*av_init_packet(&tranPkt);
	tranPkt.data = NULL;
	tranPkt.size = 0;*/
	
	avformat_alloc_output_context2(&ofmtCtx,NULL,"mp2",out_filename);

	AVOutputFormat *ofmt = NULL;
	ofmt = ofmtCtx->oformat;
//----------------------------open camera----------------------------------------------------
	AVInputFormat *ifmt = av_find_input_format("video4linux2");
	if (avformat_open_input(&ifmtCtx, "/dev/video0", ifmt, NULL) != 0)
	{
		printf("Couldn't open input stream.\n");
		goto end;
		//return -1;
	}
	if (avformat_find_stream_info(ifmtCtx, NULL)<0)
	{
		printf("Couldn't find stream information.\n");
		goto end;
		//return -1;
	}
	for (int i = 0; i < ifmtCtx->nb_streams; i++)
	{
		if (ifmtCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO)
		{
			videoindex = i;
			break;
		}
	}
	if (videoindex == -1)
	{
		printf("Couldn't find a video stream.\n");
		goto end;
		//return -1;
	}

// --------------------------find index of video in stream,         begin decode  ----------------------------------

	pDeCodecCtx = ifmtCtx->streams[videoindex]->codec;
	pDeCodec = avcodec_find_decoder(pDeCodecCtx->codec_id);
	pEnCodec = avcodec_find_encoder(en_codec_id);
	if (pDeCodec == NULL || pEnCodec == NULL )
	{
		printf("Codec not found.\n");
		goto end;
		//return -1;
	}
	pEnCodecCtx = avcodec_alloc_context3(pEnCodec);
	if (!pEnCodecCtx)
	{
		printf("Could not allocate video codec context\n");
		goto end;
		//return -1;
	}
	if (avcodec_open2(pDeCodecCtx, pDeCodec, NULL)<0)
	{
		printf("Could not open Decodec.\n");
		goto end;
		//return -1;
	}
	pFrame = av_frame_alloc();
	pFrameYUV = av_frame_alloc();
	pEnCodecCtx->width = pDeCodecCtx->width;
	pEnCodecCtx->height = pDeCodecCtx->height;
	pEnCodecCtx->bit_rate = 400000;
	pEnCodecCtx->time_base.num = 1;
	pEnCodecCtx->time_base.den = 25;
	pEnCodecCtx->max_b_frames = 1;
	pEnCodecCtx->gop_size = 250;
	pEnCodecCtx->pix_fmt = AV_PIX_FMT_YUV420P;
	if (avcodec_open2(pEnCodecCtx, pEnCodec, NULL) < 0)
	{
		printf("Could not open Encodec.\n");
		goto end;
		//return -1;
	}
	if ((ret = initFilter(pDeCodecCtx,pEnCodecCtx)) < 0)
	{
		goto end;
		//return -1;
	}
	//----------------------
	out_stream = avformat_new_stream(ofmtCtx,pDeCodecCtx->codec);
	out_stream->codec = pEnCodecCtx;
	out_stream->codec->codec_tag = 0;
	if(ofmtCtx->oformat->flags & AVFMT_GLOBALHEADER)
	{
		out_stream->codec->flags |= CODEC_FLAG_GLOBAL_HEADER;
	}
	av_dump_format(ofmtCtx,0,out_filename,1);
	if(!(ofmt->flags & AVFMT_NOFILE))
	{
		ret = avio_open(&ofmtCtx->pb,out_filename,AVIO_FLAG_WRITE);
		if(ret<0)
		{
			printf("could not open output URL");
			goto end;
			//return -1;
		}
	}
	avformat_write_header(ofmtCtx,NULL);

//-0----------------------------------------------------------------------------SDL----------------------------
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER))
	{
		printf("Could not initialize SDL - %s\n", SDL_GetError());
		goto end;
		//return -1;
	}
	SDL_Surface *screen;
	screen_w = pDeCodecCtx->width;
	screen_h = pDeCodecCtx->height;                                          //get width and height
	screen = SDL_SetVideoMode(screen_w, screen_h, 0, 0);
	if (!screen) 
	{
		printf("SDL: could not set video mode - exiting:%s\n", SDL_GetError());
		goto end;
		//return -1;
	}
	bmp = SDL_CreateYUVOverlay(screen_w, screen_h, SDL_YV12_OVERLAY, screen);
	SDL_Rect rect;
	rect.x = 0;
	rect.y = 0;
	rect.w = screen_w;
	rect.h = screen_h;

	struct SwsContext *img_convert_ctx;
	img_convert_ctx = sws_getContext(pDeCodecCtx->width, pDeCodecCtx->height, pDeCodecCtx->pix_fmt, 640, 480, AV_PIX_FMT_YUV420P, SWS_BICUBIC, NULL, NULL, NULL);
	video_tid = SDL_CreateThread(sfp_refresh_thread, NULL);
	SDL_WM_SetCaption("Simplest FFmpeg Read Camera", NULL);
	SDL_Event event;

	//------------------------------------------------------------SDL End---------------------------------------------------

	while(1)
	{
		//Wait
		//
		SDL_WaitEvent(&event);
		if (event.type == SFM_REFRESH_EVENT) 
		{
			if (av_read_frame(ifmtCtx, &tranPkt) >= 0)
			{
				if (tranPkt.stream_index == videoindex&&(filter_ctx->filter_graph))
				{
					//tranPkt.dts = av_rescale_q_rnd(tranPkt.dts, ifmtCtx->streams[videoindex]->time_base, pDeCodecCtx->time_base, (AVRounding)(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
					//tranPkt.pts = av_rescale_q_rnd(tranPkt.pts, ifmtCtx->streams[videoindex]->time_base, pDeCodecCtx->time_base, (AVRounding)(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
					ret = avcodec_decode_video2(pDeCodecCtx, pFrame, &got_frame, &tranPkt);
					if (ret < 0)
					{
						printf("Decode Error.\n");
						goto end;
						//return -1;
					}
					if (got_frame)
					{
						nFrameCount++;
						if(nFrameCount==10)
						{
							break;
						}

						ret = filterOutputFrame(pFrame,pEnCodecCtx);

						SDL_LockYUVOverlay(bmp);
						pFrameYUV->pts = av_frame_get_best_effort_timestamp(pFrameYUV);
					
						pFrameYUV->data[0] = bmp->pixels[0];
						pFrameYUV->data[1] = bmp->pixels[2];
						pFrameYUV->data[2] = bmp->pixels[1];
						pFrameYUV->linesize[0] = bmp->pitches[0];
						pFrameYUV->linesize[1] = bmp->pitches[2];
						pFrameYUV->linesize[2] = bmp->pitches[1];
						sws_scale(img_convert_ctx, (const unsigned char* const*)pFrame->data, pFrame->linesize, 0, pDeCodecCtx->height, pFrameYUV->data, pFrameYUV->linesize);
						SDL_UnlockYUVOverlay(bmp);
						SDL_DisplayYUVOverlay(bmp, &rect);
					}
				}
				av_free_packet(&tranPkt);
			}
			else 
			{
				thread_exit = 1;
			}
		}
		else if (event.type == SDL_QUIT) 
		{
			thread_exit = 1;
		}
		else if (event.type == SFM_BREAK_EVENT) 
		{
			break;
		}
	}
	av_write_trailer(ofmtCtx);
	//av_free_packet(&tranPkt);
end:
	sws_freeContext(img_convert_ctx);
	SDL_Quit();
	avcodec_close(pDeCodecCtx);
	avfilter_graph_free(&filter_ctx->filter_graph);
	av_free(filter_ctx);
	av_free(pFrameYUV);
	av_free(pFrame);
	avcodec_close(pEnCodecCtx);
	avformat_close_input(&ifmtCtx);
	avformat_free_context(ofmtCtx);
	return 0;
}
