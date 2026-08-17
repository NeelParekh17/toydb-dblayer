#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "tbl.h"
#include "codec.h"
#include "../pflayer/pf.h"

#define FREE_SPACE_OFFSET 0
#define SLOT_COUNT_OFFSET 2
#define SLOT_ARRAY_OFFSET 4

#define checkerr(err) {if (err < 0) {PF_PrintError(); exit(EXIT_FAILURE);}}

int getNumSlots(byte *pageBuf) {
    return (int)DecodeShort(pageBuf + SLOT_COUNT_OFFSET);
}

void setNumSlots(byte *pageBuf, int nslots) {
    EncodeShort((short)nslots, pageBuf + SLOT_COUNT_OFFSET);
}

int getNthSlotOffset(int slot, char *pageBuf) {
    return (int)DecodeShort((byte*)(pageBuf + SLOT_ARRAY_OFFSET + slot * 2));
}

int getLen(int slot, byte *pageBuf) {
    int numSlots = getNumSlots(pageBuf);
    if (slot < 0 || slot >= numSlots) {
        return -1;
    }
    int slotOffset = getNthSlotOffset(slot, (char*)pageBuf);
    int prevBoundary = (slot == 0) ? PF_PAGE_SIZE : getNthSlotOffset(slot - 1, (char*)pageBuf);
    return prevBoundary - slotOffset;
}

/**
 Opens a paged file, creating one if it doesn't exist, and optionally
 overwriting it.
 Returns 0 on success and a negative error code otherwise.
 If successful, it returns an initialized Table*.
 */
int
Table_Open(char *dbname, Schema *schema, bool overwrite, Table **ptable)
{
    PF_Init();
    
    if (overwrite) {
        PF_DestroyFile(dbname);
        int err = PF_CreateFile(dbname);
        if (err < 0) {
            return err;
        }
    }
    
    int fileDesc = PF_OpenFile(dbname);
    if (fileDesc < 0) {
        return fileDesc;
    }
    
    Table *table = (Table *)malloc(sizeof(Table));
    if (!table) {
        PF_CloseFile(fileDesc);
        return -1;
    }
    
    table->schema = schema;
    table->fileDesc = fileDesc;
    
    *ptable = table;
    return 0;
}

void
Table_Close(Table *tbl) {
    if (tbl) {
        int err = PF_CloseFile(tbl->fileDesc);
        checkerr(err);
        free(tbl);
    }
}

int
Table_Insert(Table *tbl, byte *record, int len, RecId *rid) {
    int err;
    int pageNum = -1;
    char *pageBuf = NULL;
    bool foundSpace = false;
    
    // Search existing pages for available space
    err = PF_GetFirstPage(tbl->fileDesc, &pageNum, &pageBuf);
    while (err >= 0) {
        int numSlots = getNumSlots((byte*)pageBuf);
        short freeSpaceOffset = DecodeShort((byte*)(pageBuf + FREE_SPACE_OFFSET));
        int slotHeaderSize = SLOT_ARRAY_OFFSET + (numSlots + 1) * 2;
        int availableSpace = freeSpaceOffset - slotHeaderSize;
        
        if (availableSpace >= len) {
            foundSpace = true;
            break;
        }
        
        int prevPage = pageNum;
        err = PF_GetNextPage(tbl->fileDesc, &pageNum, &pageBuf);
        PF_UnfixPage(tbl->fileDesc, prevPage, FALSE);
    }
    
    if (!foundSpace) {
        // Allocate a new page if no existing page had enough space
        err = PF_AllocPage(tbl->fileDesc, &pageNum, &pageBuf);
        checkerr(err);
        
        setNumSlots((byte*)pageBuf, 0);
        short freeSpaceOffset = PF_PAGE_SIZE;
        EncodeShort(freeSpaceOffset, (byte*)(pageBuf + FREE_SPACE_OFFSET));
    }
    
    // Insert record into the fixed page
    int numSlots = getNumSlots((byte*)pageBuf);
    short freeSpaceOffset = DecodeShort((byte*)(pageBuf + FREE_SPACE_OFFSET));
    
    // Place record at bottom of free space
    freeSpaceOffset -= len;
    memcpy(pageBuf + freeSpaceOffset, record, len);
    
    // Update slot array and header metadata
    EncodeShort(freeSpaceOffset, (byte*)(pageBuf + SLOT_ARRAY_OFFSET + numSlots * 2));
    numSlots++;
    setNumSlots((byte*)pageBuf, numSlots);
    EncodeShort(freeSpaceOffset, (byte*)(pageBuf + FREE_SPACE_OFFSET));
    
    // Unfix page as dirty
    err = PF_UnfixPage(tbl->fileDesc, pageNum, TRUE);
    checkerr(err);
    
    *rid = (pageNum << 16) | (numSlots - 1);
    return 0;
}

/*
 Given an rid, fill in the record (but at most maxlen bytes).
 Returns the number of bytes copied.
 */
int
Table_Get(Table *tbl, RecId rid, byte *record, int maxlen) {
    int slot = rid & 0xFFFF;
    int pageNum = rid >> 16;
    
    char *pageBuf;
    int err = PF_GetThisPage(tbl->fileDesc, pageNum, &pageBuf);
    checkerr(err);
    
    int numSlots = getNumSlots((byte*)pageBuf);
    if (slot < 0 || slot >= numSlots) {
        PF_UnfixPage(tbl->fileDesc, pageNum, FALSE);
        return -1;
    }
    
    int slotOffset = getNthSlotOffset(slot, pageBuf);
    int prevBoundary = (slot == 0) ? PF_PAGE_SIZE : getNthSlotOffset(slot - 1, pageBuf);
    int len = prevBoundary - slotOffset;
    
    int copyLen = (len < maxlen) ? len : maxlen;
    memcpy(record, pageBuf + slotOffset, copyLen);
    
    err = PF_UnfixPage(tbl->fileDesc, pageNum, FALSE);
    checkerr(err);
    
    return copyLen;
}

void
Table_Scan(Table *tbl, void *callbackObj, ReadFunc callbackfn) {
    int err;
    int pageNum = -1;
    char *pageBuf;
    
    err = PF_GetFirstPage(tbl->fileDesc, &pageNum, &pageBuf);
    if (err < 0) {
        return;
    }
    
    while (err >= 0) {
        int numSlots = getNumSlots((byte*)pageBuf);
        for (int slot = 0; slot < numSlots; slot++) {
            int slotOffset = getNthSlotOffset(slot, pageBuf);
            int prevBoundary = (slot == 0) ? PF_PAGE_SIZE : getNthSlotOffset(slot - 1, pageBuf);
            int len = prevBoundary - slotOffset;
            RecId rid = (pageNum << 16) | slot;
            callbackfn(callbackObj, rid, (byte*)(pageBuf + slotOffset), len);
        }
        
        int prevPage = pageNum;
        err = PF_GetNextPage(tbl->fileDesc, &pageNum, &pageBuf);
        PF_UnfixPage(tbl->fileDesc, prevPage, FALSE);
    }
}