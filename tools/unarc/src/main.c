#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include "platform_detection.h"
#include "spng.h"

#define VERSION_MAJOR 1
#define VERSION_MINOR 0
#define VERSION_PATCHLEVEL 0

int packFile(FILE*, const char*);
int unpackFile(FILE*, const char*);
void decompressDSC(uint8_t *out_buf, const uint8_t *file_buf);
int encodeImage(FILE* outFile, uint8_t* pixels, int width, int height, int bpp);

static void print_header()
{
	printf("Unarc - A tool from the OpenBGI package\n\n");
}

static void print_usage(const char *prog_name)
{
	printf("Usage: %s [OPTIONS]\n\n", prog_name);
	printf("Options:\n");
	printf("  -h, --help           Show this help message and exit\n");
	printf("  -V, --version        Show version information and exit\n");
	printf("  -u, --unpack         Unpack ARC file from input file into output path\n");
	printf("  -p, --pack           Pack ARC file from input file into output file\n");
	printf("  -i, --input <path>   Specify input file (ARC PackFile, or PackList text file)\n");
	printf("  -o, --output <path>  Specify output path or file\n");
	printf("\n");
	printf("Examples:\n");
	printf("  %s -p -i input.txt -o output.arc\n", prog_name);
}

static void print_version()
{
	printf("unarc %d.%d.%d\n", VERSION_MAJOR, VERSION_MINOR, VERSION_PATCHLEVEL);
	printf("Built with %s version %d.%d.%d on %s (%s), %s, %s\n",
		COMPILER_NAME,
		COMPILER_VERSION_MAJOR,
		COMPILER_VERSION_MINOR,
		COMPILER_VERSION_PATCH,
		PLATFORM_NAME,
		ARCH_NAME,
		__DATE__,
		__TIME__
	);
}

int main(int argc, char* argv[])
{
	const char* prog_name = argv[0];
	int pack = 0;
	int unpack = 0;
	const char* output_file = NULL;
	const char* input_file = NULL;
	char file_buffer[4096];

	if(argc == 1)
	{
		print_header();
		print_usage(prog_name);
		return EXIT_SUCCESS;
	}

	// Parse command-line arguments
	for(int i = 1; i < argc; i++)
	{
		const char* arg = argv[i];

		if(strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0)
		{
			print_header();
			print_usage(prog_name);
			return EXIT_SUCCESS;
		}
		else if(strcmp(arg, "-V") == 0 || strcmp(arg, "--version") == 0)
		{
			print_version();
			return EXIT_SUCCESS;
		}
		else if(strcmp(arg, "-u") == 0 || strcmp(arg, "--unpack") == 0)
		{
			unpack = 1;
		}
		else if(strcmp(arg, "-p") == 0 || strcmp(arg, "--pack") == 0)
		{
			pack = 1;
		}
		else if(strcmp(arg, "-o") == 0 || strcmp(arg, "--output") == 0)
		{
			// Option requires an argument
			if(i + 1 < argc)
			{
				output_file = argv[++i];
			}
			else
			{
				fprintf(stderr, "Error: %s requires an argument\n", arg);
				print_usage(prog_name);
				return EXIT_FAILURE;
			}
		}
		else if(strcmp(arg, "-i") == 0 || strcmp(arg, "--input") == 0)
		{
			// Option requires an argument
			if(i + 1 < argc)
			{
				input_file = argv[++i];
			}
			else
			{
				fprintf(stderr, "Error: %s requires an argument\n", arg);
				print_usage(prog_name);
				return EXIT_FAILURE;
			}
		}
		else
		{
			// Unknown option
			fprintf(stderr, "Error: Unknown option '%s'\n", arg);
			print_usage(prog_name);
			return EXIT_FAILURE;
		}
	}

	if(pack && unpack)
	{
		// Conflicting arguments
		fprintf(stderr, "Error: Conflicting arguments: Pack and Unpack options both set\n");
		print_usage(prog_name);
		return EXIT_FAILURE;
	}

	if(input_file == NULL)
	{
		// Conflicting arguments
		fprintf(stderr, "Error: No input file specified\n");
		print_usage(prog_name);
		return EXIT_FAILURE;
	}

	FILE* fptr = fopen(input_file, "r");
	if(fptr == NULL)
	{
		fprintf(stderr, "Error: Could not open input file.\n");
		return EXIT_FAILURE;
	}

	// Read header
	if(fread(file_buffer, 1, 8, fptr) != 8)
	{
		fclose(fptr);
		fprintf(stderr, "Error: Failed to read from input file: %s.\n", feof(fptr) ? "Read error" : "Unexpected end-of-file reached");
		return EXIT_FAILURE;
	}
	rewind(fptr);

	// Determine if this is a PackFile or PackList
	if(memcmp("PackFile", file_buffer, 8) == 0)
	{
		if(pack)
		{
			fclose(fptr);
			fprintf(stderr, "Error: Pack option set, but input file is a PackFile\n");
			return EXIT_FAILURE;
		}
		unpack = 1;
	}
	else if(memcmp("PackList", file_buffer, 8) == 0)
	{
		if(unpack)
		{
			fclose(fptr);
			fprintf(stderr, "Error: Unpack option set, but input file is a PackList\n");
			return EXIT_FAILURE;
		}
		pack = 1;
	}

	// Execute pack/unpack
	if(pack)
	{
		fclose(fptr);
		fprintf(stderr, "Error: Not implemented\n");
		return EXIT_FAILURE;
	}
	else if(unpack)
	{
		// Construct output path
		char* output = NULL;
		if(output_file != NULL)
		{
			size_t flen = strlen(output_file) + 1;
			output = (char*)malloc(flen);
			if(output == NULL)
			{
				fclose(fptr);
				fprintf(stderr, "Error: Failed to allocate memory for output file path\n");
				return EXIT_FAILURE;
			}
			memcpy(output, output_file, flen);
		}
		else
		{
			size_t flen = strlen(input_file);
			output = (char*)malloc(flen + 5);
			if(output == NULL)
			{
				fclose(fptr);
				fprintf(stderr, "Error: Failed to allocate memory for output file path\n");
				return EXIT_FAILURE;
			}
			memcpy(output, input_file, flen);
			output[flen + 0] = '.';
			output[flen + 1] = 'd';
			output[flen + 2] = 'i';
			output[flen + 3] = 'r';
			output[flen + 4] = 0;
		}

		// Create output dir
		if(mkdir(output, 0777) == -1)
		{
			if(errno == EEXIST)
			{
				fprintf(stderr, "Error: Directory '%s' already exists.\n", output);
			}
			else
			{
				perror("Failed to create directory");
			}
			fclose(fptr);
			free(output);
			return EXIT_FAILURE;
		}

		if(unpackFile(fptr, output) != 0)
		{
			free(output);
			fclose(fptr);
			fprintf(stderr, "Error: Unpack failed\n");
			return EXIT_FAILURE;
		}
		free(output);
	}

	fclose(fptr);
	return EXIT_SUCCESS;
}

void reportError(FILE* fptr)
{
	if(feof(fptr))
		fprintf(stderr, "Error: Unexpected end-of-file in input file.\n");
	else if(ferror(fptr))
		perror("Error: Error occured reading input file:");
	else
		fprintf(stderr, "Error: Unknown error occured while reading input file.\n");
}

int unpackFile(FILE* fptr, const char* outputDir)
{
	// Make packlist file
	size_t pathLen = strlen(outputDir);
	const char* packListName = "packlist.txt";
	char* packListPath = (char*)malloc(pathLen + 14);
	if(packListPath == NULL)
	{
		fprintf(stderr, "Error: Failed to allocate memory for PackList file path\n");
    	return 1;
	}
	memcpy(packListPath, outputDir, pathLen);
	packListPath[pathLen] = '/';
	memcpy(packListPath + pathLen + 1, packListName, 12);
	packListPath[pathLen + 13] = 0;

	FILE* packList = fopen(packListPath, "w");
	if(packList == NULL)
	{
		fprintf(stderr, "Error: Could not create PackList file at '%s'.\n", packListPath);
		free(packListPath);
		return 1;
	}
	free(packListPath);

	if(fprintf(packList, "PackList\n") < 0)
	{
		fprintf(stderr, "Error: Failed to write to PackList file.\n");
		fclose(packList);
		return 1;
	}

	// Read arc file
	fseek(fptr, 12, SEEK_SET);
	uint32_t fileCount;
	if(fread(&fileCount, 4, 1, fptr) != 1)
	{
		reportError(fptr);
		fclose(packList);
		return 1;
	}
	printf("File count: %d\n", fileCount);
	off_t dataOffset = (fileCount * 32) + 16;

	char fnameBuf[17];
	fnameBuf[16] = 0;
	for(uint32_t i = 0; i < fileCount; i++)
	{
		fseek(fptr, (i * 32) + 16, SEEK_SET);

		char* format = "raw";
		uint32_t offset;
		uint32_t size;
		if(fread(&fnameBuf, 1, 16, fptr) != 16)
		{
			reportError(fptr);
			fclose(packList);
			return 1;
		}
		if(fread(&offset, 4, 1, fptr) != 1)
		{
			reportError(fptr);
			fclose(packList);
			return 1;
		}
		if(fread(&size, 4, 1, fptr) != 1)
		{
			reportError(fptr);
			fclose(packList);
			return 1;
		}

		uint8_t* fileBuf = (uint8_t*)malloc(size);
		if(fileBuf == NULL)
		{
			fprintf(stderr, "Error: Failed to allocate %d bytes for file %d, '%s'\n", size, i + 1, fnameBuf);
			fclose(packList);
	    	return 1;
		}

		fseek(fptr, dataOffset + offset, SEEK_SET);
		if(fread(fileBuf, 1, size, fptr) != size)
		{
			reportError(fptr);
			fclose(packList);
			return 1;
		}

		int nameLen = strlen(fnameBuf);
		char* outFilePath = (char*)malloc(pathLen + nameLen + 2);
		if(outFilePath == NULL)
		{
			fprintf(stderr, "Error: Failed to allocate memory for output file path\n");
			fclose(packList);
	    	return 1;
		}
		memcpy(outFilePath, outputDir, pathLen);
		outFilePath[pathLen] = '/';
		memcpy(outFilePath + pathLen + 1, fnameBuf, nameLen);
		outFilePath[pathLen + nameLen + 1] = 0;
		FILE* outFile = fopen(outFilePath, "w");
		if(outFile == NULL)
		{
			fprintf(stderr, "Error: Could not create output file at '%s'.\n", outFilePath);
			free(outFilePath);
			fclose(packList);
			return 1;
		}
		free(outFilePath);

		if(memcmp("DSC FORMAT 1.00", fileBuf, 15) == 0)
		{
			format = "DSC1";
			size_t decompressedSize = *(uint32_t*)(fileBuf + 20);
			uint8_t* decompBuf = (uint8_t*)malloc(decompressedSize);
			if(decompBuf == NULL)
			{
				fprintf(stderr, "Error: Failed to allocate %ld bytes for DSC decompression\n", decompressedSize);
				free(fileBuf);
				fclose(outFile);
				fclose(packList);
		    	return 1;
			}

			// 10
			// 15F90F

			decompressDSC(decompBuf, fileBuf);

			uint16_t width = *(uint16_t*)(&decompBuf[0]);
			uint16_t height = *(uint16_t*)(&decompBuf[2]);
			uint8_t bpp = decompBuf[4];
			if(bpp == 8 || bpp == 24 || bpp == 32)
			{

				if(encodeImage(outFile, decompBuf + 16, width, height, bpp) != 0)
				{
					free(fileBuf);
					free(decompBuf);
					fclose(outFile);
					fclose(packList);
					return 1;
				}

				/*
				uint32_t bmpSize = (width * height * (bpp >> 3)) + 50;
				uint8_t bmpHeader[54];
				bmpHeader[0] = 'B';
				bmpHeader[1] = 'M';
				// The size of the BMP file in bytes 
				bmpHeader[2] = bmpSize & 0xFF;
				bmpHeader[3] = (bmpSize >> 8) & 0xFF;
				bmpHeader[4] = (bmpSize >> 16) & 0xFF;
				bmpHeader[5] = (bmpSize >> 24) & 0xFF;
				// Reserved
				bmpHeader[6] = 0;
				bmpHeader[7] = 0;
				// Reserved
				bmpHeader[8] = 0;
				bmpHeader[9] = 0;
				// The offset, i.e. starting address, of the byte where the bitmap image data (pixel array) can be found. 
				bmpHeader[10] = 54;
				bmpHeader[11] = 0;
				bmpHeader[12] = 0;
				bmpHeader[13] = 0;
				// 14 4 the size of this header, in bytes (40)
				bmpHeader[14] = 40;
				bmpHeader[15] = 0;
				bmpHeader[16] = 0;
				bmpHeader[17] = 0;
				// 18 4 the bitmap width in pixels (signed integer)
				bmpHeader[18] = width & 0xFF;
				bmpHeader[19] = (width >> 8) & 0xFF;
				bmpHeader[20] = 0;
				bmpHeader[21] = 0;
				// 22 4 the bitmap height in pixels (signed integer)
				bmpHeader[22] = height & 0xFF;
				bmpHeader[23] = (height >> 8) & 0xFF;
				bmpHeader[24] = 0;
				bmpHeader[25] = 0;
				// 26 2 the number of color planes (must be 1)
				bmpHeader[26] = 1;
				bmpHeader[27] = 0;
				// 28 2 the number of bits per pixel, which is the color depth of the image. Typical values are 1, 4, 8, 16, 24 and 32.
				bmpHeader[28] = bpp;
				bmpHeader[29] = 0;
				// 30 4 the compression method being used. See the next table for a list of possible values
				bmpHeader[30] = 0;
				bmpHeader[31] = 0;
				bmpHeader[32] = 0;
				bmpHeader[33] = 0;
				// 34 4 the image size. This is the size of the raw bitmap data; a dummy 0 can be given for BI_RGB bitmaps.
				bmpHeader[34] = 0;
				bmpHeader[35] = 0;
				bmpHeader[36] = 0;
				bmpHeader[37] = 0;
				// 38 4 the horizontal resolution of the image. (pixel per metre, signed integer)
				bmpHeader[38] = 0x13;
				bmpHeader[39] = 0x0B;
				bmpHeader[40] = 0;
				bmpHeader[41] = 0;
				// 42 4 the vertical resolution of the image. (pixel per metre, signed integer)
				bmpHeader[42] = 0x13;
				bmpHeader[43] = 0x0B;
				bmpHeader[44] = 0;
				bmpHeader[45] = 0;
				// 46 4 the number of colors in the color palette, or 0 to default to 2n
				bmpHeader[46] = 0;
				bmpHeader[47] = 0;
				bmpHeader[48] = 0;
				bmpHeader[49] = 0;
				// 50 4 the number of important colors used, or 0 when every color is important; generally ignored 
				bmpHeader[50] = 0;
				bmpHeader[51] = 0;
				bmpHeader[52] = 0;
				bmpHeader[53] = 0;

				if(fwrite(&bmpHeader[0], 50, 1, outFile) != 1)
				{
					reportError(outFile);
					free(fileBuf);
					free(decompBuf);
					fclose(outFile);
					fclose(packList);
					return 1;
				}
				if(fwrite(decompBuf + 16, decompressedSize - 16, 1, outFile) != 1)
				{
					reportError(outFile);
					free(fileBuf);
					free(decompBuf);
					fclose(outFile);
					fclose(packList);
					return 1;
				}
				*/
			}
			else
			{
				if(fwrite(decompBuf, decompressedSize, 1, outFile) != 1)
				{
					reportError(outFile);
					free(fileBuf);
					free(decompBuf);
					fclose(outFile);
					fclose(packList);
					return 1;
				}
			}
			free(decompBuf);
		}
		else
		{
			if(fwrite(fileBuf, size, 1, outFile) != 1)
			{
				reportError(outFile);
				free(fileBuf);
				fclose(outFile);
				fclose(packList);
				return 1;
			}
		}

		fclose(outFile);
		free(fileBuf);

		if(fprintf(packList, "%s %s\n", fnameBuf, format) < 0)
		{
			fprintf(stderr, "Error: Failed to write to PackList file.\n");
			fclose(packList);
			return 1;
		}

		printf("%d: %s (%d bytes, %s)\n", i + 1, fnameBuf, size, format);
	}

	fclose(packList);
	return 0;
}

int encodeImage(FILE* outFile, uint8_t* pixels, int width, int height, int bpp)
{
	size_t pixelsSize = width * height * (bpp >> 3);

	// Swap channels
	if(bpp == 24 || bpp == 32)
	{
		int pixelBytes = bpp >> 3;
		int pixelCount = width * height * pixelBytes;
		for(int i = 0; i < pixelCount; i += pixelBytes)
		{
			uint8_t t = pixels[i + 0];
			pixels[i + 0] = pixels[i + 2];
			pixels[i + 1] = pixels[i + 1];
			pixels[i + 2] = t;
		}
	}

	// Encode PNG
    spng_ctx* ctx = NULL;
    ctx = spng_ctx_new(SPNG_CTX_ENCODER);
    spng_set_png_file(ctx, outFile);

    struct spng_ihdr ihdr = {0};
    ihdr.width = width;
    ihdr.height = height;
    ihdr.bit_depth = 8;
    switch(bpp)
    {
    	case 8: ihdr.color_type = SPNG_COLOR_TYPE_GRAYSCALE; break;
    	case 24: ihdr.color_type = SPNG_COLOR_TYPE_TRUECOLOR; break;
    	case 32: ihdr.color_type = SPNG_COLOR_TYPE_TRUECOLOR_ALPHA; break;
    	default:
    		fprintf(stderr, "Error: Unsupported bit depth.\n");
    		return 1;
    }
    spng_set_ihdr(ctx, &ihdr);

    int ret = spng_encode_image(ctx, pixels, pixelsSize, SPNG_FMT_PNG, SPNG_ENCODE_FINALIZE);
    if(ret)
    {
        fprintf(stderr, "Error: spng_encode_image() error: %s\n", spng_strerror(ret));
        spng_ctx_free(ctx);
        return 1;
    }

    spng_ctx_free(ctx);
    return 0;
}
