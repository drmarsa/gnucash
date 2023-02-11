/********************************************************************\
 * gnc-imp-props-tx.cpp - import transactions from csv files        *
 *                                                                  *
 * This program is free software; you can redistribute it and/or    *
 * modify it under the terms of the GNU General Public License as   *
 * published by the Free Software Foundation; either version 2 of   *
 * the License, or (at your option) any later version.              *
 *                                                                  *
 * This program is distributed in the hope that it will be useful,  *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of   *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the    *
 * GNU General Public License for more details.                     *
 *                                                                  *
 * You should have received a copy of the GNU General Public License*
 * along with this program; if not, contact:                        *
 *                                                                  *
 * Free Software Foundation           Voice:  +1-617-542-5942       *
 * 51 Franklin Street, Fifth Floor    Fax:    +1-617-542-2652       *
 * Boston, MA  02110-1301,  USA       gnu@gnu.org                   *
 *                                                                  *
\********************************************************************/

#include <glib.h>
#include <glib/gi18n.h>

#include <platform.h>
#if PLATFORM(WINDOWS)
#include <windows.h>
#endif

#include "engine-helpers.h"
#include "gnc-csv-account-map.h"
#include "gnc-ui-util.h"
#include "Account.h"
#include "Transaction.h"
#include "gnc-pricedb.h"
#include <gnc-exp-parser.h>

#include <algorithm>
#include <exception>
#include <map>
#include <numeric>
#include <string>
#include <vector>

#include <boost/locale.hpp>
#include <boost/regex.hpp>
#include <boost/regex/icu.hpp>
#include <gnc-locale-utils.hpp>
#include "gnc-imp-props-tx.hpp"

namespace bl = boost::locale;

G_GNUC_UNUSED static QofLogModule log_module = GNC_MOD_IMPORT;

/* This map contains a set of strings representing the different column types. */
std::map<GncTransPropType, const char*> gnc_csv_col_type_strs = {
        { GncTransPropType::NONE, N_("None") },
        { GncTransPropType::UNIQUE_ID, N_("Transaction ID") },
        { GncTransPropType::DATE, N_("Date") },
        { GncTransPropType::NUM, N_("Number") },
        { GncTransPropType::DESCRIPTION, N_("Description") },
        { GncTransPropType::NOTES, N_("Notes") },
        { GncTransPropType::COMMODITY, N_("Transaction Commodity") },
        { GncTransPropType::VOID_REASON, N_("Void Reason") },
        { GncTransPropType::ACTION, N_("Action") },
        { GncTransPropType::ACCOUNT, N_("Account") },
        { GncTransPropType::AMOUNT, N_("Amount") },
        { GncTransPropType::AMOUNT_NEG, N_("Amount (Negated)") },
        { GncTransPropType::PRICE, N_("Price") },
        { GncTransPropType::MEMO, N_("Memo") },
        { GncTransPropType::REC_STATE, N_("Reconciled") },
        { GncTransPropType::REC_DATE, N_("Reconcile Date") },
        { GncTransPropType::TACTION, N_("Transfer Action") },
        { GncTransPropType::TACCOUNT, N_("Transfer Account") },
        { GncTransPropType::T_AMOUNT, N_("Transfer Amount") },
        { GncTransPropType::T_AMOUNT_NEG, N_("Transfer Amount (Negated)") },
        { GncTransPropType::TMEMO, N_("Transfer Memo") },
        { GncTransPropType::TREC_STATE, N_("Transfer Reconciled") },
        { GncTransPropType::TREC_DATE, N_("Transfer Reconcile Date") }
};

/* Below two vectors define which properties the user *can't* select
 * in two-split or multi-split mode (mostly because they don't make
 * sense in that context).
 */
std::vector<GncTransPropType> twosplit_blacklist = {
        GncTransPropType::UNIQUE_ID };
std::vector<GncTransPropType> multisplit_blacklist = {
        GncTransPropType::TACTION,
        GncTransPropType::TACCOUNT,
        GncTransPropType::T_AMOUNT,
        GncTransPropType::T_AMOUNT_NEG,
        GncTransPropType::TMEMO,
        GncTransPropType::TREC_STATE,
        GncTransPropType::TREC_DATE
};
/* List of properties that can be assigned to multiple columns at once */
std::vector<GncTransPropType> multi_col_props = {
    GncTransPropType::AMOUNT,
    GncTransPropType::AMOUNT_NEG,
    GncTransPropType::T_AMOUNT,
    GncTransPropType::T_AMOUNT_NEG
};

bool is_multi_col_prop (GncTransPropType prop)
{
    return (std::find (multi_col_props.cbegin(),
                       multi_col_props.cend(), prop) != multi_col_props.cend());
}

GncTransPropType sanitize_trans_prop (GncTransPropType prop, bool multi_split)
{
    auto bl = multi_split ? multisplit_blacklist : twosplit_blacklist;
    if (std::find(bl.begin(), bl.end(), prop) == bl.end())
        return prop;
    else
        return GncTransPropType::NONE;
}


/** Convert str into a GncRational using the user-specified (import) currency format.
 * @param str The string to be parsed
 * @param currency_format The currency format to use.
 * @return a GncNumeric
 * @exception May throw std::invalid argument if string can't be parsed properly
 */
GncNumeric parse_monetary (const std::string &str, int currency_format)
{
    /* An empty field is treated as zero */
    if (str.empty())
        return GncNumeric{};

    /* Strings otherwise containing no digits will be considered invalid */
    if(!boost::regex_search(str, boost::regex("[0-9]")))
        throw std::invalid_argument (_("Value doesn't appear to contain a valid number."));

    auto expr = boost::make_u32regex("[[:Sc:]]");
    std::string str_no_symbols = boost::u32regex_replace(str, expr, "");

    /* Convert based on user chosen currency format */
    gnc_numeric val = gnc_numeric_zero();
    char *endptr;
    switch (currency_format)
    {
    case 0:
        /* Currency locale */
        if (!(xaccParseAmountPosSign (str_no_symbols.c_str(), TRUE, &val, &endptr, TRUE)))
            throw std::invalid_argument (_("Value can't be parsed into a number using the selected currency format."));
        break;
    case 1:
        /* Currency decimal period */
        if (!(xaccParseAmountExtended (str_no_symbols.c_str(), TRUE, '-', '.', ',', "$+", &val, &endptr)))
            throw std::invalid_argument (_("Value can't be parsed into a number using the selected currency format."));
        break;
    case 2:
        /* Currency decimal comma */
        if (!(xaccParseAmountExtended (str_no_symbols.c_str(), TRUE, '-', ',', '.', "$+", &val, &endptr)))
            throw std::invalid_argument (_("Value can't be parsed into a number using the selected currency format."));
        break;
    }

    return GncNumeric(val);
}

static char parse_reconciled (const std::string& reconcile)
{
    if (g_strcmp0 (reconcile.c_str(), gnc_get_reconcile_str(NREC)) == 0) // Not reconciled
        return NREC;
    else if (g_strcmp0 (reconcile.c_str(), gnc_get_reconcile_str(CREC)) == 0) // Cleared
        return CREC;
    else if (g_strcmp0 (reconcile.c_str(), gnc_get_reconcile_str(YREC)) == 0) // Reconciled
        return YREC;
    else if (g_strcmp0 (reconcile.c_str(), gnc_get_reconcile_str(FREC)) == 0) // Frozen
        return FREC;
    else if (g_strcmp0 (reconcile.c_str(), gnc_get_reconcile_str(VREC)) == 0) // Voided will be handled at the transaction level
        return NREC;                                                          // so return not reconciled here
    else
        throw std::invalid_argument (_("Value can't be parsed into a valid reconcile state."));
}

gnc_commodity* parse_commodity (const std::string& comm_str)
{
    if (comm_str.empty())
        return nullptr;

    auto table = gnc_commodity_table_get_table (gnc_get_current_book());
    gnc_commodity* comm = nullptr;

    /* First try commodity as a unique name, returns null if not found */
    comm = gnc_commodity_table_lookup_unique (table, comm_str.c_str());

    /* Then try mnemonic in the currency namespace */
    if (!comm)
        comm = gnc_commodity_table_lookup (table,
                GNC_COMMODITY_NS_CURRENCY, comm_str.c_str());

    if (!comm)
    {
        /* If that fails try mnemonic in all other namespaces */
        auto namespaces = gnc_commodity_table_get_namespaces(table);
        for (auto ns = namespaces; ns; ns = ns->next)
        {
            gchar* ns_str = (gchar*)ns->data;
            if (g_utf8_collate(ns_str, GNC_COMMODITY_NS_CURRENCY) == 0)
                continue;

            comm = gnc_commodity_table_lookup (table,
                    ns_str, comm_str.c_str());
            if (comm)
                break;
        }
    }

    if (!comm)
        throw std::invalid_argument (_("Value can't be parsed into a valid commodity."));
    else
        return comm;
}

void GncPreTrans::set (GncTransPropType prop_type, const std::string& value)
{
    try
    {
        // Drop any existing error for the prop_type we're about to set
        m_errors.erase(prop_type);

        gnc_commodity *comm = nullptr;
        switch (prop_type)
        {
            case GncTransPropType::UNIQUE_ID:
                m_differ = boost::none;
                if (!value.empty())
                    m_differ = value;
                break;

            case GncTransPropType::DATE:
                m_date = boost::none;
                if (!value.empty())
                    m_date = GncDate(value, GncDate::c_formats[m_date_format].m_fmt); // Throws if parsing fails
                else if (!m_multi_split)
                    throw std::invalid_argument (
                        (bl::format (std::string{_("Date field can not be empty if 'Multi-split' option is unset.\n")}) %
                                     std::string{_(gnc_csv_col_type_strs[prop_type])}).str());
                break;

            case GncTransPropType::NUM:
                m_num = boost::none;
                if (!value.empty())
                    m_num = value;
                break;

            case GncTransPropType::DESCRIPTION:
                m_desc = boost::none;
                if (!value.empty())
                    m_desc = value;
                else if (!m_multi_split)
                    throw std::invalid_argument (
                        (bl::format (std::string{_("Description field can not be empty if 'Multi-split' option is unset.\n")}) %
                                     std::string{_(gnc_csv_col_type_strs[prop_type])}).str());
                break;

            case GncTransPropType::NOTES:
                m_notes = boost::none;
                if (!value.empty())
                    m_notes = value;
                break;

            case GncTransPropType::COMMODITY:
                m_commodity = boost::none;
                comm = parse_commodity (value); // Throws if parsing fails
                if (comm)
                    m_commodity = comm;
                break;

            case GncTransPropType::VOID_REASON:
                m_void_reason = boost::none;
                if (!value.empty())
                    m_void_reason = value;
                break;

            default:
                /* Issue a warning for all other prop_types. */
                PWARN ("%d is an invalid property for a transaction", static_cast<int>(prop_type));
                break;
        }
    }
    catch (const std::invalid_argument& e)
    {
        auto err_str = (bl::format (std::string{_("{1}: {2}")}) %
                        std::string{_(gnc_csv_col_type_strs[prop_type])} %
                        e.what()).str();
        m_errors.emplace(prop_type, err_str);
        throw std::invalid_argument (err_str);
    }
    catch (const std::out_of_range& e)
    {
        auto err_str = (bl::format (std::string{_("{1}: {2}")}) %
                        std::string{_(gnc_csv_col_type_strs[prop_type])} %
                        e.what()).str();
        m_errors.emplace(prop_type, err_str);
        throw std::invalid_argument (err_str);
    }

}

void GncPreTrans::reset (GncTransPropType prop_type)
{
    try
    {
        set (prop_type, std::string());
    }
    catch (...)
    {
        // Set with an empty string will effectively clear the property
        // but can also set an error for the property. Clear that error here.
        m_errors.erase(prop_type);
    }
}

StrVec GncPreTrans::verify_essentials (void)
{
    auto errors = StrVec();

    if (!m_date)
        errors.emplace_back(_("No valid date."));

    if (!m_desc)
        errors.emplace_back(_("No valid description."));

    return errors;
}

std::shared_ptr<DraftTransaction> GncPreTrans::create_trans (QofBook* book, gnc_commodity* currency)
{
    if (created)
        return nullptr;

    /* Gently refuse to create the transaction if the basics are not set correctly
     * This should have been tested before calling this function though!
     */
    auto check = verify_essentials();
    if (!check.empty())
    {
        auto err_msg = std::string("Not creating transaction because essentials not set properly:");
        auto add_bullet_item = [](std::string& a, std::string& b)->std::string { return std::move(a) + "\n• " + b; };
        err_msg = std::accumulate (check.begin(), check.end(), std::move (err_msg), add_bullet_item);
        PWARN ("%s", err_msg.c_str());
        return nullptr;
    }

    auto trans = xaccMallocTransaction (book);
    xaccTransBeginEdit (trans);

    if (m_commodity && gnc_commodity_is_currency(*m_commodity))
        xaccTransSetCurrency (trans, *m_commodity);
    else
        xaccTransSetCurrency (trans, currency);
    xaccTransSetDatePostedSecsNormalized (trans,
                        static_cast<time64>(GncDateTime(*m_date, DayPart::neutral)));

    if (m_num)
        xaccTransSetNum (trans, m_num->c_str());

    if (m_desc)
        xaccTransSetDescription (trans, m_desc->c_str());

    if (m_notes)
        xaccTransSetNotes (trans, m_notes->c_str());

    created = true;
    return std::make_shared<DraftTransaction>(trans);
}

bool GncPreTrans::is_part_of (std::shared_ptr<GncPreTrans> parent)
{
    if (!parent)
        return false;

    return (!m_differ || m_differ == parent->m_differ) &&
            (!m_date || m_date == parent->m_date) &&
            (!m_num || m_num == parent->m_num) &&
            (!m_desc || m_desc == parent->m_desc) &&
            (!m_notes || m_notes == parent->m_notes) &&
            (!m_commodity || m_commodity == parent->m_commodity) &&
            (!m_void_reason || m_void_reason == parent->m_void_reason) &&
            parent->m_errors.empty(); // A GncPreTrans with errors can never be a parent
}

static StrVec gen_err_strvec (ErrMap& errors, bool check_accts_mapped = false)
{
    auto full_error = StrVec();

    auto add_err = [check_accts_mapped](StrVec a, const ErrPair& b)->StrVec
                              { if (!check_accts_mapped ||
                                          ((b.first != GncTransPropType::ACCOUNT) &&
                                           (b.first != GncTransPropType::TACCOUNT)))
                                    a.emplace_back(b.second);
                                return a; };
    full_error = std::accumulate (errors.cbegin(), errors.cend(), full_error, add_err);
    return full_error;
}

ErrMap GncPreTrans::errors ()
{
    return m_errors;
}

void GncPreSplit::set (GncTransPropType prop_type, const std::string& value)
{
    try
    {
        // Drop any existing error for the prop_type we're about to set
        m_errors.erase(prop_type);

        Account *acct = nullptr;
        switch (prop_type)
        {
            case GncTransPropType::ACTION:
                m_action = boost::none;
                if (!value.empty())
                    m_action = value;
                break;

            case GncTransPropType::TACTION:
                m_taction = boost::none;
                if (!value.empty())
                    m_taction = value;
                break;

            case GncTransPropType::ACCOUNT:
                m_account = boost::none;
                if (value.empty())
                    throw std::invalid_argument (_("Account value can't be empty."));
                if ((acct = gnc_csv_account_map_search (value.c_str())) ||
                    (acct = gnc_account_lookup_by_full_name (gnc_get_current_root_account(), value.c_str())))
                    m_account = acct;
                else
                    throw std::invalid_argument (_("Account value can't be mapped back to an account."));
                break;

            case GncTransPropType::TACCOUNT:
                m_taccount = boost::none;
                if (value.empty())
                    throw std::invalid_argument (_("Transfer account value can't be empty."));

                if ((acct = gnc_csv_account_map_search (value.c_str())) ||
                    (acct = gnc_account_lookup_by_full_name (gnc_get_current_root_account(), value.c_str())))
                    m_taccount = acct;
                else
                    throw std::invalid_argument (_("Transfer account value can't be mapped back to an account."));
                break;

            case GncTransPropType::MEMO:
                m_memo = boost::none;
                if (!value.empty())
                    m_memo = value;
                break;

            case GncTransPropType::TMEMO:
                m_tmemo = boost::none;
                if (!value.empty())
                    m_tmemo = value;
                break;

            case GncTransPropType::AMOUNT:
                m_amount = boost::none;
                m_amount = parse_monetary (value, m_currency_format); // Will throw if parsing fails
                break;

            case GncTransPropType::AMOUNT_NEG:
                m_amount_neg = boost::none;
                m_amount_neg = parse_monetary (value, m_currency_format); // Will throw if parsing fails
                break;

            case GncTransPropType::T_AMOUNT:
                m_tamount = boost::none;
                m_tamount = parse_monetary (value, m_currency_format); // Will throw if parsing fails
                break;

            case GncTransPropType::T_AMOUNT_NEG:
                m_tamount_neg = boost::none;
                m_tamount_neg = parse_monetary (value, m_currency_format); // Will throw if parsing fails
                break;

            case GncTransPropType::PRICE:
                /* Note while a price is not stricly a currency, it will likely use
                 * the same decimal point as currencies in the csv file, so parse
                 * using the same parser */
                m_price = boost::none;
                m_price = parse_monetary (value, m_currency_format); // Will throw if parsing fails
                break;

            case GncTransPropType::REC_STATE:
                m_rec_state = boost::none;
                m_rec_state = parse_reconciled (value); // Throws if parsing fails
                break;

            case GncTransPropType::TREC_STATE:
                m_trec_state = boost::none;
                m_trec_state = parse_reconciled (value); // Throws if parsing fails
                break;

            case GncTransPropType::REC_DATE:
                m_rec_date = boost::none;
                if (!value.empty())
                    m_rec_date = GncDate (value,
                                          GncDate::c_formats[m_date_format].m_fmt); // Throws if parsing fails
                break;

            case GncTransPropType::TREC_DATE:
                m_trec_date = boost::none;
                if (!value.empty())
                    m_trec_date = GncDate (value,
                                           GncDate::c_formats[m_date_format].m_fmt); // Throws if parsing fails
                break;

            default:
                /* Issue a warning for all other prop_types. */
                PWARN ("%d is an invalid property for a split", static_cast<int>(prop_type));
                break;
        }
    }
    catch (const std::invalid_argument& e)
    {
        auto err_str = (bl::format (std::string{_("{1}: {2}")}) %
                        std::string{_(gnc_csv_col_type_strs[prop_type])} %
                        e.what()).str();
        m_errors.emplace(prop_type, err_str);
        throw std::invalid_argument (err_str);
    }
    catch (const std::out_of_range& e)
    {
        auto err_str = (bl::format (std::string{_("{1}: {2}")}) %
                        std::string{_(gnc_csv_col_type_strs[prop_type])} %
                        e.what()).str();
        m_errors.emplace(prop_type, err_str);
        throw std::invalid_argument (err_str);
    }
}

void GncPreSplit::reset (GncTransPropType prop_type)
{
    try
    {
        set (prop_type, std::string());
    }
    catch (...)
    {
        // Set with an empty string will effectively clear the property
        // but can also set an error for the property. Clear that error here.
        m_errors.erase(prop_type);
    }
}

void GncPreSplit::add (GncTransPropType prop_type, const std::string& value)
{
    try
    {
        // Drop any existing error for the prop_type we're about to add to
        m_errors.erase(prop_type);

        auto num_val = GncNumeric();
        switch (prop_type)
        {
            case GncTransPropType::AMOUNT:
                num_val = parse_monetary (value, m_currency_format); // Will throw if parsing fails
                if (m_amount)
                    num_val += *m_amount;
                m_amount = num_val;
                break;

            case GncTransPropType::AMOUNT_NEG:
                num_val = parse_monetary (value, m_currency_format); // Will throw if parsing fails
                if (m_amount_neg)
                    num_val += *m_amount_neg;
                m_amount_neg = num_val;
                break;

            case GncTransPropType::T_AMOUNT:
                num_val = parse_monetary (value, m_currency_format); // Will throw if parsing fails
                if (m_tamount)
                    num_val += *m_tamount;
                m_tamount = num_val;
                break;

            case GncTransPropType::T_AMOUNT_NEG:
                num_val = parse_monetary (value, m_currency_format); // Will throw if parsing fails
                if (m_tamount_neg)
                    num_val += *m_tamount_neg;
                m_tamount_neg = num_val;
                break;

            default:
                /* Issue a warning for all other prop_types. */
                PWARN ("%d can't be used to add values in a split", static_cast<int>(prop_type));
                break;
        }
    }
    catch (const std::invalid_argument& e)
    {
        auto err_str = (bl::format (std::string{_("{1}: {2}")}) %
                        std::string{_(gnc_csv_col_type_strs[prop_type])} %
                        e.what()).str();
        m_errors.emplace(prop_type, err_str);
        throw std::invalid_argument (err_str);
    }
    catch (const std::out_of_range& e)
    {
        auto err_str = (bl::format (std::string{_("{1}: {2}")}) %
                        std::string{_(gnc_csv_col_type_strs[prop_type])} %
                        e.what()).str();
        m_errors.emplace(prop_type, err_str);
        throw std::invalid_argument (err_str);
    }
}

StrVec GncPreSplit::verify_essentials()
{
    auto err_msg = StrVec();
    /* Make sure this split has the minimum required set of properties defined. */
    if (!m_amount && !m_amount_neg)
        err_msg.emplace_back (_("No amount or negated amount column."));

    if (m_rec_state && *m_rec_state == YREC && !m_rec_date)
        err_msg.emplace_back (_("Split is reconciled but reconcile date column is missing or invalid."));

    if (m_trec_state && *m_trec_state == YREC && !m_trec_date)
        err_msg.emplace_back (_("Transfer split is reconciled but transfer reconcile date column is missing or invalid."));

    return err_msg;
}

/** Adds a split to a transaction.
 * @param trans The transaction to add a split to
 * @param account The split's account
 * @param amount The split's amount
 * @param value The split's value
 * @param rec_state The split's reconcile status
 * @param rec_date The split's reconcile date
 */
static void trans_add_split (Transaction* trans, Account* account,
                            GncNumeric amount, GncNumeric value,
                            const boost::optional<std::string>& action,
                            const boost::optional<std::string>& memo,
                            const boost::optional<char>& rec_state,
                            const boost::optional<GncDate>& rec_date)
{
    QofBook* book = xaccTransGetBook (trans);
    auto split = xaccMallocSplit (book);
    xaccSplitSetAccount (split, account);
    xaccSplitSetParent (split, trans);
    xaccSplitSetAmount (split, static_cast<gnc_numeric>(amount));
    xaccSplitSetValue (split, static_cast<gnc_numeric>(value));

    if (memo)
        xaccSplitSetMemo (split, memo->c_str());
    /* Note, this function assumes the num/action switch is done at a higher level
     * if needed by the book option */
    if (action)
        xaccSplitSetAction (split, action->c_str());

    if (rec_state && *rec_state != 'n')
        xaccSplitSetReconcile (split, *rec_state);
    if (rec_state && *rec_state == YREC && rec_date)
        xaccSplitSetDateReconciledSecs (split,
                static_cast<time64>(GncDateTime(*rec_date, DayPart::neutral)));

}

void GncPreSplit::create_split (std::shared_ptr<DraftTransaction> draft_trans)
{
    if (created)
        return;

    /* Gently refuse to create the split if the basics are not set correctly
     * This should have been tested before calling this function though!
     */
    auto check = verify_essentials();
    if (!check.empty())
    {
        auto err_msg = std::string("Not creating split because essentials not set properly:");
        auto add_bullet_item = [](std::string& a, std::string& b)->std::string { return std::move(a) + "\n• " + b; };
        err_msg = std::accumulate (check.begin(), check.end(), std::move (err_msg), add_bullet_item);
        PWARN ("%s", err_msg.c_str());
        return;
    }

    auto splits_created = 0;
    Account *account = nullptr;
    Account *taccount = nullptr;
    auto amount = GncNumeric();

    if (m_account)
        account = *m_account;
    if (m_taccount)
        taccount = *m_taccount;
    if (m_amount)
        amount += *m_amount;
    if (m_amount_neg)
        amount -= *m_amount_neg;

    boost::optional<GncNumeric> tamount;
    if (m_tamount || m_tamount_neg)
    {
        tamount = GncNumeric();
        if (m_tamount)
            *tamount += *m_tamount;
        if (m_tamount_neg)
            *tamount -= *m_tamount_neg;
    }

    auto value = GncNumeric();
    auto trans_curr = xaccTransGetCurrency(draft_trans->trans);
    auto acct_comm = xaccAccountGetCommodity(account);
    if (gnc_commodity_equiv(trans_curr, acct_comm))
        value = amount;
    else if ((m_tamount || m_tamount_neg) &&
              m_taccount &&
              gnc_commodity_equiv (trans_curr, xaccAccountGetCommodity( *m_taccount)))
        value = -*tamount;
    else if (m_price)
        value = amount * *m_price;
    else
    {
        QofBook* book = xaccTransGetBook (draft_trans->trans);
        auto time = xaccTransRetDatePosted (draft_trans->trans);
        /* Import data didn't specify price, let's lookup the nearest in time */
        auto nprice =
        gnc_pricedb_lookup_nearest_in_time64(gnc_pricedb_get_db(book),
                                             acct_comm, trans_curr, time);
        if (nprice)
        {
            /* Found a usable price. Let's check if the conversion direction is right
             * Reminder: value = amount * price, or amount = value / price */
            GncNumeric rate = gnc_price_get_value(nprice);
            if (gnc_commodity_equiv(gnc_price_get_currency(nprice), trans_curr))
                value = amount * rate;
            else
                value = amount * rate.inv();
        }
        else
            PERR("No price found, can't create this split.");
    }

    /* Add a split with the cumulative amount value. */
    trans_add_split (draft_trans->trans, account, amount, value, m_action, m_memo, m_rec_state, m_rec_date);
    splits_created++;

    if (taccount)
    {
        /* If a taccount is set that forcibly means we're processing a single-line transaction
         * Determine the transfer amount. If the csv data had columns for it, use those, otherwise
         * try to calculate it. The easiest is the single-currency case: just use the negated
         * amount. In case of multi-currency, attempt to get a price and work from there. */
        auto tvalue = -value;
        if (!tamount)
        {
            auto trans_curr = xaccTransGetCurrency(draft_trans->trans);
            auto acct_comm = xaccAccountGetCommodity(taccount);
            if (gnc_commodity_equiv(trans_curr, acct_comm))
                tamount = tvalue;
            else if (m_price)
                tamount = tvalue * m_price->inv();
            else
            {
                QofBook* book = xaccTransGetBook (draft_trans->trans);
                auto time = xaccTransRetDatePosted (draft_trans->trans);
                /* Import data didn't specify price, let's lookup the nearest in time */
                auto nprice =
                gnc_pricedb_lookup_nearest_in_time64(gnc_pricedb_get_db(book),
                                                    acct_comm, trans_curr, time);
                if (nprice)
                {
                    /* Found a usable price. Let's check if the conversion direction is right
                     * Reminder: value = amount * price, or amount = value / price */
                    GncNumeric rate = gnc_price_get_value(nprice);
                    if (gnc_commodity_equiv(gnc_price_get_currency(nprice), trans_curr))
                        tamount = tvalue * rate.inv();
                    else
                        tamount = tvalue * rate;
                }
                else
                    PWARN("No price found, defer creation of second split to generic import matcher.");
            }
        }
        if (tamount)
        {
            trans_add_split (draft_trans->trans, taccount, *tamount, tvalue, m_taction, m_tmemo, m_trec_state, m_trec_date);
            splits_created++;
        }
    }

    if (splits_created == 1)
    {
        /* If we get here, we're either
         * - in multi-line mode
         * - or single-line mode but didn't have enough details to create the
         *   transfer split.
         * For the latter we will pass what we know about the transfer split to
         * allow the generic import matcher to ask the user for the final
         * details before creating this split.
         */
        draft_trans->m_price = m_price;
        draft_trans->m_taction = m_taction;
        draft_trans->m_tmemo = m_tmemo;
        draft_trans->m_tamount = tamount;
        draft_trans->m_taccount = m_taccount;
        draft_trans->m_trec_state = m_trec_state;
        draft_trans->m_trec_date = m_trec_date;
    }

    created = true;
}

ErrMap GncPreSplit::errors (void)
{
    return m_errors;
}
