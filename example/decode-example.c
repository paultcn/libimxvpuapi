/* example for how to use the imxvpuapi decoder interface
 * Copyright (C) 2014 Carlos Rafael Giani
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301
 * USA
 */


#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <stdarg.h>

#include "imxvpuapi/imxvpuapi.h"
#include "h264_utils.h"



/* This is a simple example of how to decode with the imxvpuapi library.
 * It decodes h.264 byte stream data and dumps the decoded frames to a file.
 * Also look into imxvpuapi.h for documentation. */



typedef struct
{
	char *infn, *outfn;
	FILE *fin, *fout;

	h264_context h264_ctx;

	ImxVpuDecoder *vpudec;

	ImxVpuDMABuffer *bitstream_buffer;
	size_t bitstream_buffer_size;
	unsigned int bitstream_buffer_alignment;

	ImxVpuDecInitialInfo initial_info;

	ImxVpuFramebuffer *framebuffers;
	ImxVpuDMABuffer **fb_dmabuffers;
	unsigned int num_framebuffers;
	ImxVpuDecFramebufferSizes calculated_sizes;

	unsigned int frame_id_counter;
}
AppData;


typedef enum
{
	RETVAL_OK = 0,
	RETVAL_ERROR = 1,
	RETVAL_EOS = 2
}
Retval;



static void usage(char *progname);
static int parse_args(AppData *app_data, int argc, char **argv);



static void logging_fn(ImxVpuLogLevel level, char const *file, int const line, char const *fn, const char *format, ...)
{
	va_list args;

	char const *lvlstr = "";
	switch (level)
	{
		case IMX_VPU_LOG_LEVEL_ERROR: lvlstr = "ERROR"; break;
		case IMX_VPU_LOG_LEVEL_WARNING: lvlstr = "WARNING"; break;
		case IMX_VPU_LOG_LEVEL_INFO: lvlstr = "info"; break;
		case IMX_VPU_LOG_LEVEL_DEBUG: lvlstr = "debug"; break;
		case IMX_VPU_LOG_LEVEL_TRACE: lvlstr = "trace"; break;
		case IMX_VPU_LOG_LEVEL_LOG: lvlstr = "log"; break;
		default: break;
	}

	fprintf(stderr, "%s:%d (%s)   %s: ", file, line, fn, lvlstr);

	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);

	fprintf(stderr, "\n");
}



int init(AppData *app_data, int argc, char **argv)
{
	ImxVpuDecOpenParams open_params;


	if (!parse_args(app_data, argc, argv))
		return RETVAL_ERROR;


	app_data->fin = fopen(app_data->infn, "rb");
	if (app_data->fin == NULL)
	{
		fprintf(stderr, "Opening %s for reading failed: %s\n", app_data->infn, strerror(errno));
		return RETVAL_ERROR;
	}

	app_data->fout = fopen(app_data->outfn, "wb");
	if (app_data->fout == NULL)
	{
		fprintf(stderr, "Opening %s for writing failed: %s\n", app_data->outfn, strerror(errno));
		fclose(app_data->fin);
		return RETVAL_ERROR;
	}


	app_data->frame_id_counter = 100;


	imx_vpu_set_logging_threshold(IMX_VPU_LOG_LEVEL_TRACE);
	imx_vpu_set_logging_function(logging_fn);


	h264_ctx_init(&(app_data->h264_ctx), app_data->fin);

	open_params.codec_format = IMX_VPU_CODEC_FORMAT_H264;
	open_params.frame_width = 0;
	open_params.frame_height = 0;
	open_params.enable_frame_reordering = 1;

	imx_vpu_dec_load();
	imx_vpu_dec_get_bitstream_buffer_info(&(app_data->bitstream_buffer_size), &(app_data->bitstream_buffer_alignment));
	app_data->bitstream_buffer = imx_vpu_dma_buffer_allocate(imx_vpu_dec_get_default_allocator(), app_data->bitstream_buffer_size, app_data->bitstream_buffer_alignment, 0);
	imx_vpu_dec_open(&(app_data->vpudec), &open_params, app_data->bitstream_buffer);

	return RETVAL_OK;
}


int shutdown(AppData *app_data)
{
	unsigned int i;

	imx_vpu_dec_close(app_data->vpudec);

	free(app_data->framebuffers);
	for (i = 0; i < app_data->num_framebuffers; ++i)
		imx_vpu_dma_buffer_deallocate(app_data->fb_dmabuffers[i]);
	free(app_data->fb_dmabuffers);
	imx_vpu_dma_buffer_deallocate(app_data->bitstream_buffer);

	imx_vpu_dec_unload();

	h264_ctx_cleanup(&(app_data->h264_ctx));

	if (app_data->fout != NULL)
		fclose(app_data->fout);
	if (app_data->fin != NULL)
		fclose(app_data->fin);

	return RETVAL_OK;
}


int decode_frame(AppData *app_data)
{
	ImxVpuEncodedFrame encoded_frame;
	int ok;
	unsigned int output_code;

	if (imx_vpu_dec_is_drain_mode_enabled(app_data->vpudec))
	{
		/* In drain mode there is no input data */
		encoded_frame.data.virtual_address = NULL;
		encoded_frame.data_size = 0;
		encoded_frame.codec_data = NULL;
		encoded_frame.codec_data_size = 0;
		encoded_frame.context = NULL;

		ok = 1;
	}
	else
	{
		/* Regular mode; read input data and feed it to the decoder */

		ok = h264_ctx_read_access_unit(&(app_data->h264_ctx));

		if (app_data->h264_ctx.au_end_offset <= app_data->h264_ctx.au_start_offset)
			return RETVAL_EOS;

		encoded_frame.data.virtual_address = app_data->h264_ctx.in_buffer + app_data->h264_ctx.au_start_offset;
		encoded_frame.data_size = app_data->h264_ctx.au_end_offset - app_data->h264_ctx.au_start_offset;
		/* Codec data is out-of-band data that is typically stored in a separate space in
		 * containers for each elementary stream; h.264 byte-stream does not need it */
		encoded_frame.codec_data = NULL;
		encoded_frame.codec_data_size = 0;
		/* The frame id counter is used to give the encoded frames an example context.
		 * The context of an encoded frame is a user-defined pointer that is passed along
		 * to the corresponding decoded picture. This way, it can be determined which
		 * decoded picture is the result of what encoded frame.
		 * For example purposes (to be able to print some log output), the context
		 * is just a monotonically increasing integer. */
		encoded_frame.context = (void *)((uintptr_t)(app_data->frame_id_counter));

		fprintf(stderr, "encoded input frame:  frame id: 0x%x  size: %u byte\n", app_data->frame_id_counter, encoded_frame.data_size);
	}

	/* Perform the actual decoding */
	imx_vpu_dec_decode(app_data->vpudec, &encoded_frame, &output_code);

	/* Initial info is now available; this usually happens right after the
	 * first frame is decoded, and this is the situation where one must register
	 * output framebuffers, which the decoder then uses like a buffer pool for
	 * picking buffers to decode frame into */
	if (output_code & IMX_VPU_DEC_OUTPUT_CODE_INITIAL_INFO_AVAILABLE)
	{
		unsigned int i;

		imx_vpu_dec_get_initial_info(app_data->vpudec, &(app_data->initial_info));
		fprintf(
			stderr,
			"initial info:  size: %ux%u pixel  rate: %u/%u  min num required framebuffers: %u  interlacing: %d  width/height ratio: %f  framebuffer alignment: %u\n",
			app_data->initial_info.frame_width,
			app_data->initial_info.frame_height,
			app_data->initial_info.frame_rate_numerator,
			app_data->initial_info.frame_rate_denominator,
			app_data->initial_info.min_num_required_framebuffers,
			app_data->initial_info.interlacing,
			app_data->initial_info.width_height_ratio / 65536.0f,
			app_data->initial_info.framebuffer_alignment
		);

		app_data->num_framebuffers = app_data->initial_info.min_num_required_framebuffers;

		imx_vpu_dec_calc_framebuffer_sizes(&(app_data->initial_info), 0, 0, &(app_data->calculated_sizes));
		fprintf(
			stderr,
			"calculated sizes:  frame width&height: %dx%d  Y stride: %u  CbCr stride: %u  Y size: %u  CbCr size: %u  MvCol size: %u  total size: %u\n",
			app_data->calculated_sizes.aligned_frame_width, app_data->calculated_sizes.aligned_frame_height,
			app_data->calculated_sizes.y_stride, app_data->calculated_sizes.cbcr_stride,
			app_data->calculated_sizes.y_size, app_data->calculated_sizes.cbcr_size, app_data->calculated_sizes.mvcol_size,
			app_data->calculated_sizes.total_size
		);

		app_data->framebuffers = malloc(sizeof(ImxVpuFramebuffer) * app_data->num_framebuffers);
		app_data->fb_dmabuffers = malloc(sizeof(ImxVpuDMABuffer*) * app_data->num_framebuffers);

		for (i = 0; i < app_data->num_framebuffers; ++i)
		{
			/* Allocate a DMA buffer for each framebuffer. It is possible to specify alternate allocators;
			 * all that is required is that the allocator provides physically contiguous memory
			 * (necessary for DMA transfers) and respecs the alignment value. */
			app_data->fb_dmabuffers[i] = imx_vpu_dma_buffer_allocate(imx_vpu_dec_get_default_allocator(), app_data->calculated_sizes.total_size, app_data->initial_info.framebuffer_alignment, 0);

			/* The last parameter (the one with 0x2000 + i) is the context data for the framebuffers in the pool.
			 * It is possible to attach user-defined context data to them. Note that it is not related to the
			 * context data in en- and decoded pictures. For purposes of demonstrations, the context pointer
			 * is just a simple monotonically increasing integer. First framebuffer has context 0x2000, second 0x2001 etc. */
			imx_vpu_dec_fill_framebuffer_params(&(app_data->framebuffers[i]), &(app_data->calculated_sizes), app_data->fb_dmabuffers[i], (void*)((uintptr_t)(0x2000 + i)));
		}

		/* Actual registration is done here. From this moment on, the VPU knows which buffers to use for
		 * storing decoded pictures into. This call must not be done again until decoding is shut down or
		 * IMX_VPU_DEC_OUTPUT_CODE_INITIAL_INFO_AVAILABLE is set again. */
		imx_vpu_dec_register_framebuffers(app_data->vpudec, app_data->framebuffers, app_data->num_framebuffers);
	}

	/* A decoded picture is available for further processing. Retrieve it, do something
	 * with it, and once the picture is no longer needed, mark it as displayed. This
	 * marks it internally as available for further decoding by the VPU. */
	if (output_code & IMX_VPU_DEC_OUTPUT_CODE_DECODED_PICTURE_AVAILABLE)
	{
		ImxVpuPicture decoded_picture;
		unsigned int frame_id;
		uint8_t *mapped_virtual_address;
		imx_vpu_phys_addr_t mapped_physical_address;
		size_t num_out_byte = app_data->calculated_sizes.y_size + app_data->calculated_sizes.cbcr_size * 2;

		/* This call retrieves information about the decoded picture, including
		 * a pointer to the corresponding framebuffer structure. This must not be called more
		 * than once after IMX_VPU_DEC_OUTPUT_CODE_DECODED_PICTURE_AVAILABLE was set. */
		imx_vpu_dec_get_decoded_picture(app_data->vpudec, &decoded_picture);
		frame_id = (unsigned int)((uintptr_t)(decoded_picture.context));
		fprintf(stderr, "decoded output picture:  frame id: 0x%x  writing %u byte\n", frame_id, num_out_byte);

		/* Map buffer to the local address space, dump the decoded frame to file,
		 * and unmap again. The decoded frame uses the I420 color format for all
		 * bitstream formats (h.264, MPEG2 etc.), with one exception; with motion JPEG data,
		 * the format can be different. See imxvpuapi.h for details. */
		imx_vpu_dma_buffer_map(decoded_picture.framebuffer->dma_buffer, &mapped_virtual_address, &mapped_physical_address, IMX_VPU_MAPPING_FLAG_READ_ONLY);
		fwrite(mapped_virtual_address, 1, num_out_byte, app_data->fout);
		imx_vpu_dma_buffer_unmap(decoded_picture.framebuffer->dma_buffer);

		/* Mark the framebuffer as displayed, thus returning it to the list of
		 *framebuffers available for decoding. */
		imx_vpu_dec_mark_framebuffer_as_displayed(app_data->vpudec, decoded_picture.framebuffer);
	}
	else if (output_code & IMX_VPU_DEC_OUTPUT_CODE_DROPPED)
	{
		/* A frame was dropped. The context of the dropped frame can be retrieved
		 * if this is necessary for timestamping etc. */
		unsigned int dropped_frame_id = (unsigned int)((uintptr_t)(imx_vpu_dec_get_dropped_frame_context(app_data->vpudec)));
		fprintf(stderr, "dropped frame:  frame id: 0x%x\n", dropped_frame_id);
	}

	if (output_code & IMX_VPU_DEC_OUTPUT_CODE_EOS)
	{
		fprintf(stderr, "VPU reports EOS; no more decoded frames available");
		ok = 0;
	}

	app_data->frame_id_counter++;

	return ok ? RETVAL_OK : RETVAL_EOS;
}




int main(int argc, char *argv[])
{
	AppData app_data;
	memset(&app_data, 0, sizeof(AppData));

	/* Initialize decoder */
	if (init(&app_data, argc, argv) == RETVAL_ERROR)
		return 1;

	/* Feed frames to decoder & decode & output, until we run out of input data */
	for (;;)
	{
		Retval ret = decode_frame(&app_data);
		if (ret == RETVAL_EOS)
			break;
		else if (ret == RETVAL_ERROR)
		{
			shutdown(&app_data);
			return 1;
		}
	}

	/* Enable drain mode; in this mode, any decoded pictures that are still in the
	 * decoder are output; no input data is given (since there isn't any input data anymore) */

	fprintf(stderr, "draining decoder");
	imx_vpu_dec_enable_drain_mode(app_data.vpudec, 1);

	for (;;)
	{
		Retval ret = decode_frame(&app_data);
		if (ret == RETVAL_EOS)
			break;
		else if (ret == RETVAL_ERROR)
		{
			shutdown(&app_data);
			return 1;
		}
	}

	/* Cleanup */
	return shutdown(&app_data);
}







/*****  BOILERPLATE CODE FROM HERE ON *****/


static void usage(char *progname)
{
	static char options[] =
		"\t-i input file containing h.264 data in byte-stream format (with access unit delimiters)\n"
		"\t-o output file containing decoded YUV frames\n"
		;

	fprintf(stderr, "usage:\t%s [option]\n\noption:\n%s\n", progname, options);
}


static int parse_args(AppData *app_data, int argc, char **argv)
{
	int opt;

	while ((opt = getopt(argc, argv, "i:o:")) != -1)
	{
		switch (opt)
		{
			case 'i':
				app_data->infn = optarg;
				break;
			case 'o':
				app_data->outfn = optarg;
				break;
			default:
				usage(argv[0]);
				return 0;
		}
	}

	if (app_data->infn == NULL)
	{
		fprintf(stderr, "Missing input filename\n\n");
		usage(argv[0]);
		return 0;
	}

	if (app_data->outfn == NULL)
	{
		fprintf(stderr, "Missing output filename\n\n");
		usage(argv[0]);
		return 0;
	}

	return 1;
}
