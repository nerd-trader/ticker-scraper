#pragma once

#define true  1
#define false 0

typedef char bool;

typedef enum MarketPlaces
{
    AMEX   = (1 << 0),
    NYSE   = (1 << 1),
    NASDAQ = (1 << 2)
} MarketPlace;

typedef char Symbol[7];

typedef struct DataRow {
    MarketPlace marketplace;
    Symbol ticker;
    char company[512];
    char sector[512];
    char industry[512];
    char country[512];
    char marketcap[512];
} DataRow;

char *marketplace_to_str(MarketPlace marketplace);

int scrape_ticker_symbols(MarketPlace marketplace);
int ticker_scraper_add(DataRow *dataRow);
