#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "tbl.h"
#include "codec.h"
#include "../pflayer/pf.h"

#define SLOT_COUNT_OFFSET 2
#define checkerr(err) {if (err < 0) {PF_PrintError(); exit(EXIT_FAILURE);}}

int getLen(int slot, byte *pageBuf);

int getNumSlots(byte *pageBuf) {
    return DecodeShort(pageBuf + SLOT_COUNT_OFFSET);
}

void setNumSlots(byte *pageBuf, int nslots) {
    EncodeShort((short)nslots, pageBuf + SLOT_COUNT_OFFSET);
}

int getNthSlotOffset(int slot, char* pageBuf) {
    return DecodeShort(pageBuf + 4 + slot * 2);
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
    int err;
    
    // Initialize PF
    PF_Init();
    checkerr(err);
    
    // Create or open the file
    int fileDesc;
    if (overwrite) {
        err = PF_CreateFile(dbname);
    }
    
    fileDesc = PF_OpenFile(dbname);
    checkerr(fileDesc);
    
    // Allocate and initialize Table structure
    Table *table = (Table *)malloc(sizeof(Table));
    if (!table) {
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
        // Close the file
        int err = PF_CloseFile(tbl->fileDesc);
        checkerr(err);
        
        // Free the table structure
        free(tbl);
    }
}

int
Table_Insert(Table *tbl, byte *record, int len, RecId *rid) {
    int err;
    int pageNum;
    char *pageBuf;
    
    // Try to get the last page first to see if there's space
    err = PF_GetFirstPage(tbl->fileDesc, &pageNum, &pageBuf);
    if (err < 0) {
        // No pages exist, allocate a new one
        err = PF_AllocPage(tbl->fileDesc, &pageNum, &pageBuf);
        checkerr(err);
        
        // Initialize empty page
        setNumSlots(pageBuf, 0);
        short freeSpaceOffset = PF_PAGE_SIZE;
        EncodeShort(freeSpaceOffset, pageBuf);
    } else {
        // Check existing pages for space
        bool foundSpace = false;
        do {
            int numSlots = getNumSlots(pageBuf);
            short freeSpaceOffset = DecodeShort(pageBuf);
            int slotHeaderSize = 2 + 2 + (numSlots + 1) * 2; // numSlots + freeSpace + (numSlots+1) slot offsets
            int availableSpace = freeSpaceOffset - slotHeaderSize;
            
            if (availableSpace >= len) {
                foundSpace = true;
                break;
            }
            
            err = PF_GetNextPage(tbl->fileDesc, &pageNum, &pageBuf);
        } while (err >= 0);
        
        if (!foundSpace) {
            // Allocate a new page
            err = PF_AllocPage(tbl->fileDesc, &pageNum, &pageBuf);
            checkerr(err);
            
            // Initialize empty page
            setNumSlots(pageBuf, 0);
            short freeSpaceOffset = PF_PAGE_SIZE;
            EncodeShort(freeSpaceOffset, pageBuf);
        }
    }
    
    // Insert record into current page
    int numSlots = getNumSlots(pageBuf);
    short freeSpaceOffset = DecodeShort(pageBuf);
    
    // Place record at the bottom of free space
    freeSpaceOffset -= len;
    memcpy(pageBuf + freeSpaceOffset, record, len);
    
    // Update slot array
    EncodeShort(freeSpaceOffset, pageBuf + 4 + numSlots * 2);
    numSlots++;
    setNumSlots(pageBuf, numSlots);
    EncodeShort(freeSpaceOffset, pageBuf);
    
    // Unfix the page as dirty
    err = PF_UnfixPage(tbl->fileDesc, pageNum, TRUE);
    checkerr(err);
    
    // Create RecId
    *rid = (pageNum << 16) | (numSlots - 1);
    
    return 0;
}

#define checkerr(err) {if (err < 0) {PF_PrintError(); exit(EXIT_FAILURE);}}

/*
 Given an rid, fill in the record (but at most maxlen bytes).
 Returns the number of bytes copied.
 */
int
Table_Get(Table *tbl, RecId rid, byte *record, int maxlen) {
    int slot = rid & 0xFFFF;
    int pageNum = rid >> 16;
    
    int err;
    char *pageBuf;
    
    // Get the page
    err = PF_GetThisPage(tbl->fileDesc, pageNum, &pageBuf);
    checkerr(err);
    
    // Get the slot offset
    int slotOffset = getNthSlotOffset(slot, pageBuf);
    
    // Calculate record length
    int len;
    int numSlots = getNumSlots(pageBuf);
    if (slot == numSlots - 1) {
        // Last slot - length is from slot offset to free space pointer
        short freeSpaceOffset = DecodeShort(pageBuf);
        len = freeSpaceOffset - slotOffset;
    } else {
        // Not last slot - length is difference between this and next slot
        int nextSlotOffset = getNthSlotOffset(slot + 1, pageBuf);
        len = nextSlotOffset - slotOffset;
    }
    
    // Copy the record
    int copyLen = (len < maxlen) ? len : maxlen;
    memcpy(record, pageBuf + slotOffset, copyLen);
    
    // Unfix the page
    err = PF_UnfixPage(tbl->fileDesc, pageNum, FALSE);
    checkerr(err);
    
    return len;
}

void
Table_Scan(Table *tbl, void *callbackObj, ReadFunc callbackfn) {
    int err;
    int pageNum;
    char *pageBuf;
    
    // Get first page
    err = PF_GetFirstPage(tbl->fileDesc, &pageNum, &pageBuf);
    if (err < 0) {
        return; // No pages in file
    }
    
    do {
        int numSlots = getNumSlots(pageBuf);
        
        // For each record in this page
        for (int slot = 0; slot < numSlots; slot++) {
            int slotOffset = getNthSlotOffset(slot, pageBuf);
            
            // Calculate record length
            int len;
            if (slot == numSlots - 1) {
                // Last slot
                short freeSpaceOffset = DecodeShort(pageBuf);
                len = freeSpaceOffset - slotOffset;
            } else {
                // Not last slot
                int nextSlotOffset = getNthSlotOffset(slot + 1, pageBuf);
                len = nextSlotOffset - slotOffset;
            }
            
            // Create RecId
            RecId rid = (pageNum << 16) | slot;
            
            // Call callback function
            callbackfn(callbackObj, rid, (byte*)(pageBuf + slotOffset), len);
        }
        
        // Get next page
        err = PF_GetNextPage(tbl->fileDesc, &pageNum, &pageBuf);
    } while (err >= 0);
}