/*
 * catalog.h - Command catalog loader and whitelist (Phase 4)
 *
 * Loads catalog/win32-commands.json (a nested document the single-level
 * json_parser.c cannot handle) via a small self-contained recursive-descent
 * scanner, then exposes case-insensitive lookup and argument validation.
 * The dispatcher uses the gate semantics (catalog.allium); kind, lookup,
 * and validation live here.
 *
 * This is free and unencumbered software released into the public domain.
 * See LICENSE for details (Unlicense).
 */

#ifndef CATALOG_H
#define CATALOG_H

typedef struct CatalogEntry CatalogEntry;
typedef struct Catalog      Catalog;

/*
 * CatalogLoad - Read and parse a catalog JSON file.
 * Returns 1 on success (outCat receives a heap Catalog), 0 on failure
 * (errMsg filled). Free the result with CatalogFree.
 */
int  CatalogLoad(const char *path, Catalog **outCat, char *errMsg, int errSize);

/*
 * CatalogFree - Release a Catalog returned by CatalogLoad. NULL-safe.
 */
void CatalogFree(Catalog *cat);

/*
 * CatalogLookup - Case-insensitive lookup of a command name.
 * Returns the entry, or NULL when absent.
 */
const CatalogEntry *CatalogLookup(const Catalog *cat, const char *cmdName);

/*
 * CatalogValidateArgs - Validate argv (incl. argv[0], the command name)
 * against the entry's declared options. Flags (tokens starting '/' or '-')
 * must appear in options (case-insensitive); unknown flag -> 0 with
 * "argument not allowed". A flag whose option declares an arg consumes the
 * next token when that token is not itself a flag; the glued /A:value form
 * also validates. Positionals are advisory and always allowed.
 * Returns 1 if allowed, 0 otherwise (errMsg filled).
 */
int  CatalogValidateArgs(const CatalogEntry *entry, const char **argv,
                         int argc, char *errMsg, int errSize);

/*
 * Entry accessors the dispatcher needs.
 */
const char *CatalogEntryName(const CatalogEntry *e);
int         CatalogEntryIsBuiltin(const CatalogEntry *e);
const char *CatalogEntryShellModern(const CatalogEntry *e);
const char *CatalogEntryShellWin32s(const CatalogEntry *e);
int         CatalogEntrySupportsWin32s(const CatalogEntry *e);

/*
 * CatalogCount - Number of entries loaded.
 */
int         CatalogCount(const Catalog *cat);

#endif /* CATALOG_H */
