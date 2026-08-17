#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <ctype.h>
#include "codec.h"
#include "../pflayer/pf.h"
#include "../amlayer/am.h"
#include "tbl.h"
#include "util.h"

#define checkerr(err) {if (err < 0) {PF_PrintError(); exit(1);}}
#define MAX_PAGE_SIZE 4000
#define DB_NAME "data.db"
#define INDEX_NAME "data.db.0"
#define CSV_NAME "data.csv"

/*
Takes a schema, and an array of strings (fields), and uses the functionality
in codec.c to convert strings into compact binary representations
 */
int
encode(Schema *sch, char **fields, byte *record, int spaceLeft) {
    byte *cursor = record;
    int totalBytes = 0;
    
    for (int i = 0; i < sch->numColumns; i++) {
        int bytesUsed = 0;
        
        switch (sch->columns[i]->type) {
            case VARCHAR:
                bytesUsed = EncodeCString(fields[i], cursor, spaceLeft - totalBytes);
                break;
            case INT:
                bytesUsed = EncodeInt(atoi(fields[i]), cursor);
                break;
            case LONG:
                bytesUsed = EncodeLong(atoll(fields[i]), cursor);
                break;
        }
        
        cursor += bytesUsed;
        totalBytes += bytesUsed;
    }
    
    return totalBytes;
}

Schema *
loadCSV() {
    // Open csv file, parse schema
    FILE *fp = fopen(CSV_NAME, "r");
    if (!fp) {
        perror("data.csv could not be opened");
        exit(EXIT_FAILURE);
    }

    char buf[MAX_LINE_LEN];
    char *line = fgets(buf, MAX_LINE_LEN, fp);
    if (line == NULL) {
        fprintf(stderr, "Unable to read data.csv\n");
        exit(EXIT_FAILURE);
    }

    // Open main db file
    Schema *sch = parseSchema(line);
    Table *tbl;
    int err = Table_Open(DB_NAME, sch, true, &tbl);
    checkerr(err);
    
    // Open index file
    err = PF_CreateFile(INDEX_NAME);
    int indexFD = PF_OpenFile(INDEX_NAME);
    checkerr(indexFD);
    
    // Create index
    err = AM_CreateIndex(INDEX_NAME, 0, 'i', 4);
    checkerr(err);

    char *tokens[MAX_TOKENS];
    char record[MAX_PAGE_SIZE];
    while ((line = fgets(buf, MAX_LINE_LEN, fp)) != NULL) {
        int n = split(line, ",", tokens);
        assert (n == sch->numColumns);
        int len = encode(sch, tokens, record, sizeof(record));
        RecId rid;
        err = Table_Insert(tbl, record, len, &rid);
        checkerr(err);
        printf("%d %s\n", rid, tokens[0]);
        
        // Indexing on the population column
        int population = atoi(tokens[2]);
        err = AM_InsertEntry(indexFD, 'i', 4, (char*)&population, rid);
        checkerr(err);
    }

    fclose(fp);
    Table_Close(tbl);
    err = PF_CloseFile(indexFD);
    checkerr(err);
    return sch;
}

int
main() {
    loadCSV();
}