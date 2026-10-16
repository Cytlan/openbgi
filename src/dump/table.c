/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * table.c - the data entries of the dump and pack tools (dump_internal.h):
 *           the name and scenario tables as JSON, an SDC-coded block as
 *           its decoded bytes, and everything else as it is
 *
 * The tables of a scenario archive - "StringsDB" (in the oldest games
 * "BitmapNameDB") and "ScenarioDB" - are a record count and fixed-size
 * records of NUL-padded Shift-JIS strings and 32-bit numbers:
 *
 *   32 bytes    a number, a name of 28 bytes
 *   64 bytes    a name of 48 bytes, four numbers
 *   128 bytes   a name of 96 bytes, a second name of 28 bytes, a number
 *
 * They become {"table", "record_size", "records": [..]} with the strings
 * in UTF-8.  What the numbers mean is the game's business (the tools only
 * carry them); the names of ScenarioDB include the chapter titles a game
 * shows.  A table is written as JSON only when that JSON makes the same
 * bytes again, so an oddly filled one stays a plain file.
 *
 * An SDC block (codec.h; the tables of the bust-shot system in the later
 * games) is decoded to "<entry>.bin"; bytes in front of the SDC header, as
 * "evdb" has them, go into "<entry>.json" as "prefix" (hex) and are put
 * back unchanged.
 */
#include "dump_internal.h"
#include "bgi/codec.h"

static uint32_t Le32(const uint8_t* p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void PutLe32(uint8_t* p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

// ---- the tables ------------------------------------------------------------------------------------

// one field of a record layout
typedef struct TableField
{
	const char* key; // the member's name
	uint32_t offset; // in the record, bytes
	uint32_t size;   // a string field's size; 0 for a number
	uint32_t count;  // numbers in a row (an array member when more than one)
} TableField_t;

static const TableField_t kLayout32[] = {{"name", 4, 28, 0}, {"value", 0, 0, 1}, {NULL, 0, 0, 0}};
static const TableField_t kLayout64[] = {{"name", 0, 48, 0}, {"values", 48, 0, 4}, {NULL, 0, 0, 0}};
static const TableField_t kLayout128[] = {{"name", 0, 96, 0}, {"label", 96, 28, 0}, {"value", 124, 0, 1}, {NULL, 0, 0, 0}};

static const TableField_t* LayoutOf(uint32_t recordSize)
{
	return recordSize == 32 ? kLayout32 : recordSize == 64 ? kLayout64
		: recordSize == 128                                ? kLayout128
														   : NULL;
}

// the JSON of a table (the caller frees the text)
static void TableToJson(DumpText_t* t, const char* name, const uint8_t* data, uint32_t count, uint32_t recordSize)
{
	const TableField_t* layout = LayoutOf(recordSize);
	uint32_t i;
	DumpText_Add(t, "{\n  \"table\": ");
	DumpText_AddString(t, name);
	DumpText_Printf(t, ",\n  \"record_size\": %u,\n  \"records\": [", (unsigned)recordSize);
	for(i = 0; i < count; i++)
	{
		const uint8_t* rec = data + 4 + (size_t)i * recordSize;
		const TableField_t* f;
		DumpText_Add(t, i ? ",\n    {" : "\n    {");
		for(f = layout; f->key; f++)
		{
			DumpText_Printf(t, "%s\"%s\": ", f == layout ? "" : ", ", f->key);
			if(f->size)
			{
				char sjis[128], utf[512];
				memcpy(sjis, rec + f->offset, f->size);
				sjis[f->size] = 0;
				OS_SjisToUtf8(sjis, utf, sizeof utf);
				DumpText_AddString(t, utf);
			}
			else if(f->count == 1)
				DumpText_Printf(t, "%u", (unsigned)Le32(rec + f->offset));
			else
			{
				uint32_t k;
				DumpText_Add(t, "[");
				for(k = 0; k < f->count; k++)
					DumpText_Printf(t, "%s%u", k ? ", " : "", (unsigned)Le32(rec + f->offset + k * 4));
				DumpText_Add(t, "]");
			}
		}
		DumpText_Add(t, "}");
	}
	DumpText_Add(t, count ? "\n  ]\n}\n" : "]\n}\n");
}

uint8_t* DumpTable_FromJson(const char* text, size_t len, uint32_t* size, char* err, size_t errSize)
{
	JsonNode_t* root = Json_Parse(text, len, NULL);
	const JsonNode_t* records = Json_Get(root, "records");
	const JsonNode_t* r;
	uint32_t recordSize = (uint32_t)Json_Number(root, "record_size", 0), count, i = 0;
	const TableField_t* layout = LayoutOf(recordSize);
	uint8_t* out;
	err[0] = 0;
	if(!root || !records || records->type != JSON_ARRAY || !layout)
	{
		snprintf(err, errSize, "%s", !root ? "not JSON" : "a table needs \"record_size\" (32, 64 or 128) and \"records\"");
		Json_Free(root);
		return NULL;
	}
	count = (uint32_t)Json_Count(records);
	*size = 4 + count * recordSize;
	out = (uint8_t*)BGI_Calloc(*size);
	PutLe32(out, count);
	for(r = records->child; r; r = r->next, i++)
	{
		uint8_t* rec = out + 4 + (size_t)i * recordSize;
		const TableField_t* f;
		for(f = layout; f->key; f++)
		{
			const JsonNode_t* m = Json_Get(r, f->key);
			if(f->size)
			{
				char sjis[512];
				size_t n = OS_Utf8ToSjis(m && m->type == JSON_STRING ? m->str : "", sjis, sizeof sjis);
				if(n > f->size)
				{
					snprintf(err, errSize, "record %u: \"%s\" takes %u bytes in Shift-JIS, the field holds %u", (unsigned)i, f->key, (unsigned)n,
						(unsigned)f->size);
					BGI_Free(out);
					Json_Free(root);
					return NULL;
				}
				memcpy(rec + f->offset, sjis, n);
			}
			else if(f->count == 1)
				PutLe32(rec + f->offset, m ? (uint32_t)m->num : 0);
			else
			{
				const JsonNode_t* v = m && m->type == JSON_ARRAY ? m->child : NULL;
				uint32_t k;
				for(k = 0; k < f->count; k++, v = v ? v->next : NULL)
					PutLe32(rec + f->offset + k * 4, v ? (uint32_t)v->num : 0);
			}
		}
	}
	Json_Free(root);
	return out;
}

int Dump_Table(DumpCtx_t* c, const uint8_t* data, uint32_t size)
{
	DumpText_t t;
	uint32_t count, recordSize, backSize = 0;
	uint8_t* back;
	char rel[DUMP_NAME_MAX + 16], err[160];
	int same;
	if(!Dump_EqualNoCase(c->entryName, "StringsDB") && !Dump_EqualNoCase(c->entryName, "ScenarioDB") && !Dump_EqualNoCase(c->entryName, "BitmapNameDB"))
		return 0;
	if(size < 4)
		return 0;
	count = Le32(data);
	if(count == 0)
	{ // an empty table: the record size is not in the file; the name tables' is always 32
		if(size != 4)
			return 0;
		recordSize = Dump_EqualNoCase(c->entryName, "ScenarioDB") ? 128 : 32;
	}
	else
	{
		recordSize = (size - 4) / count;
		if((size - 4) % count || !LayoutOf(recordSize))
			return 0;
	}
	memset(&t, 0, sizeof t);
	TableToJson(&t, c->entryName, data, count, recordSize);
	back = DumpTable_FromJson(t.p, t.len, &backSize, err, sizeof err);
	same = back && backSize == size && memcmp(back, data, size) == 0;
	BGI_Free(back);
	if(!same)
	{
		Dump_Note(c, "the table does not come back from its JSON form: kept as it is");
		c->problems++;
		DumpText_Free(&t);
		return 0;
	}
	snprintf(rel, sizeof rel, "%s.json", c->base);
	Dump_EmitFile(c, "file", rel, t.p, t.len);
	DumpText_Free(&t);
	return 1;
}

uint8_t* Pack_Table(PackCtx_t* c, const JsonNode_t* entry, uint32_t* size)
{
	uint32_t len = 0;
	uint8_t* text = Pack_ReadMember(c, entry, "file", &len);
	uint8_t* out;
	char err[160];
	if(!text)
		return NULL;
	out = DumpTable_FromJson((const char*)text, len, size, err, sizeof err);
	if(!out)
		Pack_Note(c, "%s", err);
	free(text);
	return out;
}

// ---- SDC blocks ------------------------------------------------------------------------------------

#define SDC_PREFIX_MAX 8 // bytes in front of the SDC header that are still taken for a prefix

int Dump_Sdc(DumpCtx_t* c, const uint8_t* data, uint32_t size)
{
	uint32_t prefix, outSize;
	SdcHeader_t h;
	uint8_t* out;
	char rel[DUMP_NAME_MAX + 16];
	int ok;
	for(prefix = 0; prefix <= SDC_PREFIX_MAX; prefix += SDC_PREFIX_MAX)
		if(size >= prefix + sizeof h && SdcIsFormat(data + prefix))
			break;
	if(prefix > SDC_PREFIX_MAX)
		return 0;
	memcpy(&h, data + prefix, sizeof h);
	if(h.encLen != size - prefix - sizeof h) // something follows the block: not a file this understands
		return 0;
	outSize = h.outSize;
	out = (uint8_t*)BGI_Calloc((size_t)outSize + 16);
	if(SdcDecode(out, data + prefix) != outSize)
	{
		Dump_Note(c, "the SDC block does not decode: kept as it is");
		c->problems++;
		BGI_Free(out);
		return 0;
	}
	snprintf(rel, sizeof rel, "%s.bin", c->base);
	ok = Dump_EmitFile(c, "file", rel, out, outSize);
	BGI_Free(out);
	if(ok && prefix)
	{
		char hex[SDC_PREFIX_MAX * 2 + 1], json[64];
		int n;
		Dump_Hex(hex, data, prefix);
		n = snprintf(json, sizeof json, "{\n  \"prefix\": \"%s\"\n}\n", hex);
		snprintf(rel, sizeof rel, "%s.json", c->base);
		Dump_EmitFile(c, "info", rel, json, (size_t)n);
	}
	return 1;
}

uint8_t* Pack_Sdc(PackCtx_t* c, const JsonNode_t* entry, uint32_t* size)
{
	uint8_t prefix[SDC_PREFIX_MAX * 4];
	uint32_t len = 0, total;
	int prefixLen = 0;
	uint8_t *payload, *out;
	if(Json_Get(entry, "info"))
	{
		uint32_t n = 0;
		uint8_t* text = Pack_ReadMember(c, entry, "info", &n);
		JsonNode_t* info = text ? Json_Parse((const char*)text, n, NULL) : NULL;
		prefixLen = info ? Dump_Unhex(Json_String(info, "prefix", ""), prefix, sizeof prefix) : -1;
		Json_Free(info);
		free(text);
		if(prefixLen < 0)
		{
			Pack_Note(c, "the block's .json file needs a \"prefix\" of hex digits");
			return NULL;
		}
	}
	payload = Pack_ReadMember(c, entry, "file", &len);
	if(!payload)
		return NULL;
	// room for the header and the coder's worst case, which is more than the input (codec.h)
	out = (uint8_t*)BGI_Calloc((size_t)prefixLen + sizeof(SdcHeader_t) + (size_t)len * 2 + 64);
	memcpy(out, prefix, (size_t)prefixLen);
	total = SdcEncode(out + prefixLen, payload, len);
	free(payload);
	if(!total)
	{
		Pack_Note(c, "the SDC encoder failed");
		BGI_Free(out);
		return NULL;
	}
	*size = (uint32_t)prefixLen + total;
	return out;
}

// ---- plain bytes ------------------------------------------------------------------------------------

int Dump_Data(DumpCtx_t* c, const uint8_t* data, uint32_t size)
{
	Dump_EmitFile(c, "file", c->base, data, size);
	return 1;
}

uint8_t* Pack_Data(PackCtx_t* c, const JsonNode_t* entry, uint32_t* size)
{
	uint8_t* file = Pack_ReadMember(c, entry, "file", size);
	uint8_t* out;
	if(!file)
		return NULL;
	out = (uint8_t*)BGI_Alloc((size_t)*size + 1);
	memcpy(out, file, *size);
	free(file);
	return out;
}
