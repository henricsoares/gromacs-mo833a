/*
 *
 *                This source code is part of
 *
 *                 G   R   O   M   A   C   S
 *
 *          GROningen MAchine for Chemical Simulations
 *
 * Written by David van der Spoel, Erik Lindahl, Berk Hess, and others.
 * Copyright (c) 1991-2000, University of Groningen, The Netherlands.
 * Copyright (c) 2001-2009, The GROMACS development team,
 * check out http://www.gromacs.org for more information.

 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * If you want to redistribute modifications, please consider that
 * scientific software is very special. Version control is crucial -
 * bugs must be traceable. We will be happy to consider code for
 * inclusion in the official distribution, but derived work must not
 * be called official GROMACS. Details are found in the README & COPYING
 * files - if they are missing, get the official version at www.gromacs.org.
 *
 * To help us fund GROMACS development, we humbly ask that you cite
 * the papers on the package - you can find them in the top README file.
 *
 * For more info, check our website at http://www.gromacs.org
 */
/*! \internal \file
 * \brief
 * Implements gmx::SelectionCollection.
 *
 * \author Teemu Murtola <teemu.murtola@cbr.su.se>
 * \ingroup module_selection
 */
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <cstdio>

#include <smalloc.h>
#include <statutil.h>
#include <string2.h>
#include <xvgr.h>

#include "poscalc.h"
#include "selmethod.h"

#include "gromacs/fatalerror/exceptions.h"
#include "gromacs/fatalerror/gmxassert.h"
#include "gromacs/fatalerror/messagestringcollector.h"
#include "gromacs/options/basicoptions.h"
#include "gromacs/options/options.h"
#include "gromacs/selection/selection.h"
#include "gromacs/selection/selectioncollection.h"

#include "mempool.h"
#include "scanner.h"
#include "selectioncollection-impl.h"
#include "selectionoptionstorage.h"
#include "selelem.h"
#include "selmethod.h"
#include "symrec.h"

/* In parser.y */
/*! \brief
 * Parser function generated by Bison.
 */
int
_gmx_sel_yybparse(void *scanner);

namespace gmx
{

/********************************************************************
 * SelectionCollection::Impl
 */

int SelectionCollection::Impl::SelectionRequest::count() const
{
    return storage->maxValueCount();
}

SelectionCollection::Impl::Impl(gmx_ana_poscalc_coll_t *pcc)
    : _options("selection", "Common selection control"),
      _debugLevel(0), _grps(NULL)
{
    _sc.root      = NULL;
    _sc.nvars     = 0;
    _sc.varstrs   = NULL;
    _sc.top       = NULL;
    gmx_ana_index_clear(&_sc.gall);
    _sc.pcc       = pcc;
    _sc.mempool   = NULL;
    _sc.symtab    = NULL;

    // TODO: This is not exception-safe if any called function throws.
    if (_sc.pcc == NULL)
    {
        int rc = gmx_ana_poscalc_coll_create(&_sc.pcc);
        if (rc != 0)
        {
            // TODO: A more reasonable error
            GMX_THROW(InternalError("Failed to create position calculation collection"));
        }
        _flags.set(Impl::efOwnPositionCollection);
    }
    _gmx_sel_symtab_create(&_sc.symtab);
    gmx_ana_selmethod_register_defaults(_sc.symtab);
}


SelectionCollection::Impl::~Impl()
{
    _gmx_selelem_free_chain(_sc.root);
    SelectionList::const_iterator isel;
    for (isel = _sc.sel.begin(); isel != _sc.sel.end(); ++isel)
    {
        delete *isel;
    }
    for (int i = 0; i < _sc.nvars; ++i)
    {
        sfree(_sc.varstrs[i]);
    }
    sfree(_sc.varstrs);
    gmx_ana_index_deinit(&_sc.gall);
    if (_sc.mempool)
    {
        _gmx_sel_mempool_destroy(_sc.mempool);
    }
    if (hasFlag(efOwnPositionCollection))
    {
        gmx_ana_poscalc_coll_free(_sc.pcc);
    }
    clearSymbolTable();
}


void
SelectionCollection::Impl::clearSymbolTable()
{
    if (_sc.symtab)
    {
        _gmx_sel_symtab_free(_sc.symtab);
        _sc.symtab = NULL;
    }
}


void
SelectionCollection::Impl::runParser(yyscan_t scanner, int maxnr,
                                     std::vector<Selection *> *output)
{
    gmx_ana_selcollection_t *sc = &_sc;
    MessageStringCollector *errors = _gmx_sel_lexer_error_reporter(scanner);
    GMX_ASSERT(sc == _gmx_sel_lexer_selcollection(scanner),
               "Incorrectly initialized lexer");

    int oldCount = sc->sel.size();
    int bOk = !_gmx_sel_yybparse(scanner);
    _gmx_sel_free_lexer(scanner);
    int nr = sc->sel.size() - oldCount;
    if (maxnr > 0 && nr != maxnr)
    {
        bOk = false;
        errors->append("Too few selections provided");
    }

    if (bOk)
    {
        SelectionList::const_iterator i;
        for (i = _sc.sel.begin() + oldCount; i != _sc.sel.end(); ++i)
        {
            output->push_back(*i);
        }
    }

    if (!bOk || !errors->isEmpty())
    {
        GMX_ASSERT(bOk && !errors->isEmpty(), "Inconsistent error reporting");
        GMX_THROW(InvalidInputError(errors->toString()));
    }
}


void SelectionCollection::Impl::requestSelections(
        const std::string &name, const std::string &descr,
        SelectionOptionStorage *storage)
{
    _requests.push_back(SelectionRequest(name, descr, storage));
}


void SelectionCollection::Impl::resolveExternalGroups(
        t_selelem *root, MessageStringCollector *errors)
{

    if (root->type == SEL_GROUPREF)
    {
        bool bOk = true;
        if (root->u.gref.name != NULL)
        {
            char *name = root->u.gref.name;
            if (!gmx_ana_indexgrps_find(&root->u.cgrp, _grps, name))
            {
                // TODO: Improve error messages
                errors->append("Unknown group referenced in a selection");
                bOk = false;
            }
            else
            {
                sfree(name);
            }
        }
        else
        {
            if (!gmx_ana_indexgrps_extract(&root->u.cgrp, _grps,
                                           root->u.gref.id))
            {
                // TODO: Improve error messages
                errors->append("Unknown group referenced in a selection");
                bOk = false;
            }
        }
        if (bOk)
        {
            root->type = SEL_CONST;
            root->name = root->u.cgrp.name;
        }
    }

    t_selelem *child = root->child;
    while (child != NULL)
    {
        resolveExternalGroups(child, errors);
        child = child->next;
    }
}


/********************************************************************
 * SelectionCollection
 */

SelectionCollection::SelectionCollection(gmx_ana_poscalc_coll_t *pcc)
    : _impl(new Impl(pcc))
{
}


SelectionCollection::~SelectionCollection()
{
    delete _impl;
}


Options *
SelectionCollection::initOptions()
{
    static const char * const debug_levels[]
        = {"no", "basic", "compile", "eval", "full", NULL};
    /*
    static const char * const desc[] = {
        "This program supports selections in addition to traditional",
        "index files. Use [TT]-select help[tt] for additional information,",
        "or type 'help' in the selection prompt.",
        NULL,
    };
    options.setDescription(desc);
    */

    Options &options = _impl->_options;
    const char **postypes = gmx_ana_poscalc_create_type_enum(TRUE);
    if (postypes == NULL)
    {
        // TODO: Use an out-of-memory exception here
        GMX_THROW(InternalError("Could not create position calculation enum"));
    }
    options.addOption(StringOption("selrpos").enumValue(postypes + 1)
                          .store(&_impl->_rpost).defaultValue(postypes[1])
                          .description("Selection reference positions"));
    options.addOption(StringOption("seltype").enumValue(postypes + 1)
                          .store(&_impl->_spost).defaultValue(postypes[1])
                          .description("Default selection output positions"));
    GMX_RELEASE_ASSERT(_impl->_debugLevel >= 0 && _impl->_debugLevel <= 4,
                       "Debug level out of range");
    options.addOption(StringOption("seldebug").hidden(_impl->_debugLevel == 0)
                          .enumValue(debug_levels)
                          .defaultValue(debug_levels[_impl->_debugLevel])
                          .storeEnumIndex(&_impl->_debugLevel)
                          .description("Print out selection trees for debugging"));
    sfree(postypes);

    return &_impl->_options;
}


void
SelectionCollection::setReferencePosType(const char *type)
{
    GMX_RELEASE_ASSERT(type != NULL, "Cannot assign NULL position type");
    //! Check that the type is valid.
    e_poscalc_t  dummytype;
    int          dummyflags;
    gmx_ana_poscalc_type_from_enum(type, &dummytype, &dummyflags);
    _impl->_rpost = type;
}


void
SelectionCollection::setOutputPosType(const char *type)
{
    GMX_RELEASE_ASSERT(type != NULL, "Cannot assign NULL position type");
    //! Check that the type is valid.
    e_poscalc_t  dummytype;
    int          dummyflags;
    gmx_ana_poscalc_type_from_enum(type, &dummytype, &dummyflags);
    _impl->_spost = type;
}


void
SelectionCollection::setDebugLevel(int debuglevel)
{
    _impl->_debugLevel = debuglevel;
}


void
SelectionCollection::setTopology(t_topology *top, int natoms)
{
    gmx_ana_selcollection_t *sc = &_impl->_sc;
    gmx_ana_poscalc_coll_set_topology(sc->pcc, top);
    sc->top = top;

    /* Get the number of atoms from the topology if it is not given */
    if (natoms <= 0)
    {
        if (sc->top == NULL)
        {
            GMX_THROW(APIError("Selections need either the topology or the number of atoms"));
        }
        natoms = sc->top->atoms.nr;
    }

    gmx_ana_index_init_simple(&sc->gall, natoms, NULL);
}


void
SelectionCollection::setIndexGroups(gmx_ana_indexgrps_t *grps)
{
    GMX_RELEASE_ASSERT(grps == NULL || !_impl->hasFlag(Impl::efExternalGroupsSet),
                       "Can only set external groups once or clear them afterwards");
    _impl->_grps = grps;
    _impl->_flags.set(Impl::efExternalGroupsSet);

    MessageStringCollector errors;
    t_selelem *root = _impl->_sc.root;
    while (root != NULL)
    {
        _impl->resolveExternalGroups(root, &errors);
        root = root->next;
    }
    if (!errors.isEmpty())
    {
        GMX_THROW(InvalidInputError(errors.toString()));
    }
}


bool
SelectionCollection::requiresTopology() const
{
    t_selelem   *sel;
    e_poscalc_t  type;
    int          flags;

    if (!_impl->_rpost.empty())
    {
        flags = 0;
        // Should not throw, because has been checked earlier.
        gmx_ana_poscalc_type_from_enum(_impl->_rpost.c_str(), &type, &flags);
        if (type != POS_ATOM)
        {
            return TRUE;
        }
    }
    if (!_impl->_spost.empty())
    {
        flags = 0;
        // Should not throw, because has been checked earlier.
        gmx_ana_poscalc_type_from_enum(_impl->_spost.c_str(), &type, &flags);
        if (type != POS_ATOM)
        {
            return TRUE;
        }
    }

    sel = _impl->_sc.root;
    while (sel)
    {
        if (_gmx_selelem_requires_top(sel))
        {
            return TRUE;
        }
        sel = sel->next;
    }
    return FALSE;
}


class RequestsClearer
{
    public:
        RequestsClearer(SelectionCollection::Impl::RequestList *requests)
            : _requests(requests)
        {
        }
        ~RequestsClearer()
        {
            _requests->clear();
        }

    private:
        SelectionCollection::Impl::RequestList *_requests;
};


void
SelectionCollection::parseRequestedFromStdin(bool bInteractive)
{
    RequestsClearer clearRequestsOnExit(&_impl->_requests);

    Impl::RequestList::const_iterator i;
    for (i = _impl->_requests.begin(); i != _impl->_requests.end(); ++i)
    {
        const Impl::SelectionRequest &request = *i;
        if (bInteractive)
        {
            std::fprintf(stderr, "\nSpecify ");
            if (request.count() < 0)
            {
                std::fprintf(stderr, "any number of selections");
            }
            else if (request.count() == 1)
            {
                std::fprintf(stderr, "a selection");
            }
            else
            {
                std::fprintf(stderr, "%d selections", request.count());
            }
            std::fprintf(stderr, " for option '%s' (%s):\n",
                         request.name.c_str(), request.descr.c_str());
            std::fprintf(stderr, "(one selection per line, 'help' for help%s)\n",
                         request.count() < 0 ? ", Ctrl-D to end" : "");
        }
        std::vector<Selection *> selections;
        parseFromStdin(request.count(), bInteractive, &selections);
        request.storage->addSelections(selections, true);
    }
}


void
SelectionCollection::parseRequestedFromString(const std::string &str)
{
    RequestsClearer clearRequestsOnExit(&_impl->_requests);

    std::vector<Selection *> selections;
    parseFromString(str, &selections);

    std::vector<Selection *>::const_iterator first = selections.begin();
    std::vector<Selection *>::const_iterator last = first;
    Impl::RequestList::const_iterator i;
    for (i = _impl->_requests.begin(); i != _impl->_requests.end(); ++i)
    {
        const Impl::SelectionRequest &request = *i;
        if (request.count() > 0)
        {
            if (selections.end() - first < request.count())
            {
                GMX_THROW(InvalidInputError("Too few selections provided"));
            }
            last = first + request.count();
        }
        else
        {
            if (i != _impl->_requests.end() - 1)
            {
                GMX_THROW(APIError("Request for all selections not the last option"));
            }
            last = selections.end();
        }
        std::vector<Selection *> curr(first, last);
        request.storage->addSelections(curr, true);
        first = last;
    }
    if (last != selections.end())
    {
        GMX_THROW(InvalidInputError("Too many selections provided"));
    }
}


void
SelectionCollection::parseFromStdin(int nr, bool bInteractive,
                                    std::vector<Selection *> *output)
{
    yyscan_t scanner;

    _gmx_sel_init_lexer(&scanner, &_impl->_sc, bInteractive, nr,
                        _impl->hasFlag(Impl::efExternalGroupsSet),
                        _impl->_grps);
    /* We don't set the lexer input here, which causes it to use a special
     * internal implementation for reading from stdin. */
    _impl->runParser(scanner, nr, output);
}


void
SelectionCollection::parseFromFile(const std::string &filename,
                                   std::vector<Selection *> *output)
{
    yyscan_t scanner;
    FILE *fp;

    _gmx_sel_init_lexer(&scanner, &_impl->_sc, false, -1,
                        _impl->hasFlag(Impl::efExternalGroupsSet),
                        _impl->_grps);
    fp = ffopen(filename.c_str(), "r");
    _gmx_sel_set_lex_input_file(scanner, fp);
    // TODO: Use RAII
    try
    {
        _impl->runParser(scanner, -1, output);
    }
    catch (std::exception &)
    {
        ffclose(fp);
        throw;
    }
    ffclose(fp);
}


void
SelectionCollection::parseFromString(const std::string &str,
                                     std::vector<Selection *> *output)
{
    yyscan_t scanner;

    _gmx_sel_init_lexer(&scanner, &_impl->_sc, false, -1,
                        _impl->hasFlag(Impl::efExternalGroupsSet),
                        _impl->_grps);
    _gmx_sel_set_lex_input_str(scanner, str.c_str());
    _impl->runParser(scanner, -1, output);
}


void
SelectionCollection::compile()
{
    if (!_impl->hasFlag(Impl::efExternalGroupsSet))
    {
        setIndexGroups(NULL);
    }
    if (_impl->_debugLevel >= 1)
    {
        printTree(stderr, false);
    }
    gmx_ana_selcollection_compile(this);
    if (_impl->hasFlag(Impl::efOwnPositionCollection))
    {
        if (_impl->_debugLevel >= 1)
        {
            std::fprintf(stderr, "\n");
            printTree(stderr, false);
            std::fprintf(stderr, "\n");
            gmx_ana_poscalc_coll_print_tree(stderr, _impl->_sc.pcc);
            std::fprintf(stderr, "\n");
        }
        gmx_ana_poscalc_init_eval(_impl->_sc.pcc);
        if (_impl->_debugLevel >= 1)
        {
            gmx_ana_poscalc_coll_print_tree(stderr, _impl->_sc.pcc);
            std::fprintf(stderr, "\n");
        }
    }
}


void
SelectionCollection::evaluate(t_trxframe *fr, t_pbc *pbc)
{
    if (_impl->hasFlag(Impl::efOwnPositionCollection))
    {
        gmx_ana_poscalc_init_frame(_impl->_sc.pcc);
    }
    gmx_ana_selcollection_evaluate(&_impl->_sc, fr, pbc);
    if (_impl->_debugLevel >= 3)
    {
        std::fprintf(stderr, "\n");
        printTree(stderr, true);
    }
}


void
SelectionCollection::evaluateFinal(int nframes)
{
    gmx_ana_selcollection_evaluate_fin(&_impl->_sc, nframes);
}


void
SelectionCollection::printTree(FILE *fp, bool bValues) const
{
    t_selelem *sel;

    sel = _impl->_sc.root;
    while (sel)
    {
        _gmx_selelem_print_tree(fp, sel, bValues, 0);
        sel = sel->next;
    }
}


void
SelectionCollection::printXvgrInfo(FILE *out, output_env_t oenv) const
{
    int  i;

    if (output_env_get_xvg_format(oenv) != exvgNONE)
    {
        gmx_ana_selcollection_t *sc = &_impl->_sc;
        std::fprintf(out, "# Selections:\n");
        for (i = 0; i < sc->nvars; ++i)
        {
            std::fprintf(out, "#   %s\n", sc->varstrs[i]);
        }
        for (i = 0; i < (int)sc->sel.size(); ++i)
        {
            std::fprintf(out, "#   %s\n", sc->sel[i]->_sel.selstr);
        }
        std::fprintf(out, "#\n");
    }
}

} // namespace gmx
