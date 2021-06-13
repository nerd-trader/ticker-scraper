#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <curl/curl.h>
typedef unsigned int uint; /* Needed by tidy/tidyplatform.h */
typedef unsigned long ulong; /* Needed by tidy/tidy.h */
#include <tidy/tidy.h>
#include <tidy/tidybuffio.h>

#include "../config.h"
#include "../curl.h"
#include "../ticker-scraper.h"
#include "finviz.h"

#define FINVIZ_PP 20 /* How many results per page finviz.com displays */

CURL *curl_handle;
unsigned int m;
char *cursor;
int page, total, new;

void extract_text(TidyDoc doc, TidyNode tnod, char *destination, ulong max_size)
{
    TidyBuffer buf;
    tidyBufInit(&buf);
    tidyNodeGetText(doc, tnod, &buf);
    strncpy(destination, (char *)buf.bp, max_size - 1);
    if (strlen((char *)buf.bp) < max_size) {
        destination[strlen((char *)buf.bp) - 1] = '\0';
    } else {
        destination[max_size - 1] = '\0';
    }
    tidyBufFree(&buf);
}

/* Traverse the document tree */
int process_node(TidyDoc doc, TidyNode tnod, MarketPlace marketplace)
{
    TidyNode child;
    int tt = -1;

    /* Loop through children of tnod, looking for div#screener-content */
    for (child = tidyGetChild(tnod); child; child = tidyGetNext(child)) {
        ctmbstr name = tidyNodeGetName(child);

        if (name && strcmp(name, "div") == 0) {
            TidyAttr attr;
            for (attr = tidyAttrFirst(child); attr; attr = tidyAttrNext(attr)) {
                if (strcmp(tidyAttrName(attr), "id") == 0 && strcmp(tidyAttrValue(attr), "screener-content") == 0) {
                    /* Found <div id="screener-content" */
                    const TidyNode table = tidyGetChild(child);

                    /* Dig into the DOM and find "Total:" value */
                    {
                        TidyNode tr = tidyGetChild(table);
                        tr = tidyGetNext(tr);
                        tr = tidyGetNext(tr);
                        TidyNode td = tidyGetChild(tr);
                        TidyNode table = tidyGetChild(td);
                        tr = tidyGetChild(table);
                        td = tidyGetChild(tr);
                        TidyNode b = tidyGetChild(td);
                        TidyNode text = tidyGetNext(b);
                        /* Parse total items number */
                        TidyBuffer buf;
                        tidyBufInit(&buf);
                        tidyNodeGetText(doc, text, &buf);
                        int t, n;
                        n = sscanf((char *)buf.bp, " %d #", &t);
                        tt = (n == 1) ? t : 0;
                        tidyBufFree(&buf);
                    }

                    /* Go back up to main table and parse all rows */
                    {
                        TidyNode tr = tidyGetChild(table);
                        tr = tidyGetNext(tr);
                        tr = tidyGetNext(tr);
                        tr = tidyGetNext(tr);
                        TidyNode td = tidyGetChild(tr);
                        TidyNode table = tidyGetChild(td);
                        tr = tidyGetChild(table);
                        tr = tidyGetNext(tr);
                        for (; tr; tr = tidyGetNext(tr)) {
                            DataRow dataRow;
                            dataRow.marketplace = marketplace;

                            TidyNode td = tidyGetChild(tr);
                            // Skip first column
                            td = tidyGetNext(td);
                            // Parse Ticker
                            {
                                TidyNode a = tidyGetChild(td);
                                TidyNode text = tidyGetChild(a);
                                extract_text(doc, text, dataRow.ticker, sizeof(dataRow.ticker));
                            }
                            // Go to the next column
                            td = tidyGetNext(td);
                            // Parse Company
                            {
                                TidyNode a = tidyGetChild(td);
                                TidyNode text = tidyGetChild(a);
                                extract_text(doc, text, dataRow.company, sizeof(dataRow.company));
                            }
                            // Go to the next column
                            td = tidyGetNext(td);
                            // Parse Sector
                            {
                                TidyNode a = tidyGetChild(td);
                                TidyNode text = tidyGetChild(a);
                                extract_text(doc, text, dataRow.sector, sizeof(dataRow.sector));
                            }
                            // Go to the next column
                            td = tidyGetNext(td);
                            // Parse Industry
                            {
                                TidyNode a = tidyGetChild(td);
                                TidyNode text = tidyGetChild(a);
                                extract_text(doc, text, dataRow.industry, sizeof(dataRow.industry));
                            }
                            // Go to the next column
                            td = tidyGetNext(td);
                            // Parse Country
                            {
                                TidyNode a = tidyGetChild(td);
                                TidyNode text = tidyGetChild(a);
                                extract_text(doc, text, dataRow.country, sizeof(dataRow.country));
                            }
                            // Go to the next column
                            td = tidyGetNext(td);
                            // Parse Market Cap
                            {
                                TidyNode a = tidyGetChild(td);
                                TidyNode text = tidyGetChild(a);
                                extract_text(doc, text, dataRow.marketcap, sizeof(dataRow.marketcap));
                            }

                            new += ticker_scraper_add(&dataRow);
                            total++;
                        }
                    }
                }
            }
        }

        /* Dig deeper if couldn't find #screener-content above */
        if (tt <= 0) {
            tt = process_node(doc, child, marketplace);
        }
    }

    return tt;
}

int finviz_parse_page_extract_symbols(struct MemoryStruct *chunk, MarketPlace marketplace)
{
    m = 0;
    int t = 0;

    TidyDoc tdoc;
    TidyBuffer docbuf = {0};
    TidyBuffer tidy_errbuf = {0};
    int err;

    tdoc = tidyCreate();
    tidyOptSetBool(tdoc, TidyForceOutput, true); /* Try harder */
    tidyOptSetInt(tdoc, TidyWrapLen, 4096);
    tidySetErrorBuffer(tdoc, &tidy_errbuf);
    tidyBufInit(&docbuf);
    tidyBufAttach(&docbuf, (unsigned char *)chunk->memory, strlen(chunk->memory));

    err = tidyParseBuffer(tdoc, &docbuf); /* Parse the input */
    if (err >= 0) {
        err = tidyCleanAndRepair(tdoc); /* Fix problems */
        if (err >= 0) {
            err = tidyRunDiagnostics(tdoc); /* Load tidy error buffer */
            if (err >= 0) {
                t = process_node(tdoc, tidyGetRoot(tdoc), marketplace); /* Walk the tree */
#if DEBUG
                // fprintf(stderr, "%s\n", tidy_errbuf.bp); /* Display errors */
#endif
            }
        }
    }

    tidyBufFree(&docbuf);
    tidyBufFree(&tidy_errbuf);
    tidyRelease(tdoc);

    return t;
}

void finviz_scrape_page(struct MemoryStruct *chunk, MarketPlace marketplace)
{
    CURLcode res;

    chunk->memory = malloc(1); /* Will be grown as needed by realloc() in nerd_trader_curl_write_memory_callback */
    chunk->size = 0; /* No data at this point */

    /* Specify the URL */
    char url[strlen(RESOURCE_FINVIZ_SCAN_URL) + 10 + 3 + 11 + 1];
    strncpy(url, RESOURCE_FINVIZ_SCAN_URL, sizeof(url) - 1);
    url[sizeof(url) - 1] = '\0';

    switch (marketplace) {
        case AMEX:
            strncat(url, ",exch_amex", sizeof(url) - 1 - strlen(url));
        break;

        case NYSE:
            strncat(url, ",exch_nyse", sizeof(url) - 1 - strlen(url));
        break;

        case NASDAQ:
            strncat(url, ",exch_nasd", sizeof(url) - 1 - strlen(url));
        break;

        default:
            free(chunk->memory);
            return;
    }

    if (page > 1) {
        strncat(url, "&r=", sizeof(url) - 1 - strlen(url));
        char pageNum[11];
        sprintf(pageNum, "%d", (page - 1) * FINVIZ_PP + 1);
        strncat(url, pageNum,  sizeof(url) - 1 - strlen(url) - strlen(pageNum));
    }
    curl_easy_setopt(curl_handle, CURLOPT_URL, url);

    /* Get it! */
    res = curl_easy_perform(curl_handle);

#if DEBUG
    fprintf(stderr, "Scraping page %d: %s\n", page, url);
#endif

    /* Check for errors */
    if (res != CURLE_OK) {
        fprintf(stderr, "curl_easy_perform() failed: %s\n",
                        curl_easy_strerror(res));
    } else {
        int t = finviz_parse_page_extract_symbols(chunk, marketplace);
        page = (page * FINVIZ_PP < t) ? page + 1 : -1;
    }

    // free(chunk->memory);
}

int ticker_scraper_scrape_finviz(MarketPlace marketplace)
{
    struct MemoryStruct chunk;

    /* Init the curl */
    curl_handle = nerd_trader_curl_init(&chunk);

    page = 1;
    total = new = 0;

    while (page > 0) {
        /* Avoid "Too many requests." error */
        if (page > 1) {
            sleep(1);
        }

        finviz_scrape_page(&chunk, marketplace);
    }

    nerd_trader_curl_cleanup(curl_handle);

    return new;
}
