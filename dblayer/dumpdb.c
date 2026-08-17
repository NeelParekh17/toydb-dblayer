#include <stdio.h>
#include <stdlib.h>
#include "codec.h"
#include "tbl.h"
#include "util.h"
#include "../pflayer/pf.h"
#include "../amlayer/am.h"

#define checkerr(err) {if (err < 0) {PF_PrintError(); exit(1);}}

void
printRow(void *callbackObj, RecId rid, byte *row, int len) {
    Schema *schema = (Schema *) callbackObj;
    byte *cursor = row;
    
    for (int i = 0; i < schema->numColumns; i++) {
        if (i > 0) printf(",");
        
        switch (schema->columns[i]->type) {
            case VARCHAR: {
                char str[1000];
                int strLen = DecodeCString(cursor, str, sizeof(str));
                printf("%s", str);
                cursor += 2 + strLen; // 2 bytes for length + string length
                break;
            }
            case INT: {
                int value = DecodeInt(cursor);
                printf("%d", value);
                cursor += 4;
                break;
            }
            case LONG: {
                long long value = DecodeLong(cursor);
                printf("%lld", value);
                cursor += 8;
                break;
            }
        }
    }
    printf("\n");
}

#define DB_NAME "data.db"
#define INDEX_NAME "data.db.0"

void
index_scan(Table *tbl, Schema *schema, int indexFD, int op, int value) {
    int err;
    int scanDesc;
    
    // Open index scan
    scanDesc = AM_OpenIndexScan(indexFD, 'i', 4, op, (char*)&value);
    checkerr(scanDesc);
    
    // Scan through index entries
    RecId rid;
    while ((rid = AM_FindNextEntry(scanDesc)) >= 0) {
        // Fetch record from table
        byte record[4000];
        int len = Table_Get(tbl, rid, record, sizeof(record));
        
        // Print the row
        printRow(schema, rid, record, len);
    }
    
    // Close index scan
    err = AM_CloseIndexScan(scanDesc);
    checkerr(err);
}

int
main(int argc, char **argv) {
    char *schemaTxt = "Country:varchar,Capital:varchar,Population:int";
    Schema *schema = parseSchema(schemaTxt);
    Table *tbl;
    
    int err = Table_Open(DB_NAME, schema, false, &tbl);
    checkerr(err);

    if (argc == 2 && *(argv[1]) == 's') {
        // Sequential scan
        Table_Scan(tbl, schema, printRow);
    } else {
        // index scan by default
        int indexFD = PF_OpenFile(INDEX_NAME);
        checkerr(indexFD);
        
        // Ask for populations less than 100000, then more than 100000. Together they should
        // yield the complete database.
        index_scan(tbl, schema, indexFD, 4, 100000); // LESS_THAN_EQUAL = 4
        index_scan(tbl, schema, indexFD, 3, 100000); // GREATER_THAN = 3
        
        err = PF_CloseFile(indexFD);
        checkerr(err);
    }
    
    Table_Close(tbl);
}