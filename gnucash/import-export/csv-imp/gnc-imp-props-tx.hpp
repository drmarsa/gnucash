/********************************************************************\
 * gnc-imp-props-tx.hpp - encapsulate transaction properties for    *
 *                        use in the csv importer                   *
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

#ifndef GNC_TRANS_PROPS_HPP
#define GNC_TRANS_PROPS_HPP

#include <platform.h>
#if PLATFORM(WINDOWS)
#include <windows.h>
#endif

#include <glib/gi18n.h>

#include "Account.h"
#include "Transaction.h"
#include "gnc-commodity.h"

#include <string>
#include <map>
#include <memory>
#include <boost/optional.hpp>
#include <gnc-datetime.hpp>
#include <gnc-numeric.hpp>

/** Enumeration for column types. These are the different types of
 * columns that can exist in a CSV/Fixed-Width file. There should be
 * no two columns with the same type except for the GncTransPropType::NONE
 * type. */
enum class GncTransPropType {
    NONE,
    UNIQUE_ID,
    DATE,
    NUM,
    DESCRIPTION,
    NOTES,
    COMMODITY,
    VOID_REASON,
    TRANS_PROPS = VOID_REASON,

    ACTION,
    ACCOUNT,
    AMOUNT,
    AMOUNT_NEG,
    PRICE,
    MEMO,
    REC_STATE,
    REC_DATE,
    TACTION,
    TACCOUNT,
    T_AMOUNT,
    T_AMOUNT_NEG,
    TMEMO,
    TREC_STATE,
    TREC_DATE,
    SPLIT_PROPS = TREC_DATE
};

using StrVec = std::vector<std::string>;
using ErrMap = std::map<const GncTransPropType, std::string>;
using ErrPair = std::pair<const GncTransPropType, std::string>;

/** Maps all column types to a string representation.
 *  The actual definition is in gnc-csv-imp-trans.cpp.
 *  Attention: that definition should be adjusted for any
 *  changes to enum class GncTransPropType ! */
extern std::map<GncTransPropType, const char*> gnc_csv_col_type_strs;

/** Functor to check if the above map has an element of which
 *  the value equals name. To be used with std::find_if.
 */
struct test_prop_type_str
{
    test_prop_type_str( const char* name ) : m_name(name) {}
    bool operator()( const std::pair<GncTransPropType, const char*>& v ) const
    {
        return !g_strcmp0(v.second, m_name);
    }
private:
    const char *m_name;
};

/** Some properties can be assigned to more than one column.
 *  This function returns true if prop is such a property.
 */
bool is_multi_col_prop (GncTransPropType prop);

/** Some properties only make sense in a multi-split context.
 *  Inversely some also only make sense in a two-split context.
 *  Below function will test a property against a given context
 *  and will return that property if it makes sense
 *  or GncTransPropType::NONE if not.
 */
GncTransPropType sanitize_trans_prop (GncTransPropType prop, bool multi_split);


gnc_commodity* parse_commodity (const std::string& comm_str);
GncNumeric parse_monetary (const std::string &str, int currency_format);


/** The final form of a transaction to import before it is passed on to the
 *  generic importer.
 *
 *  @param trans a possibly incomplete transaction created based on the data
 *         collected from the PreTrans and PreSplit records
 *
 *  @param m_price... values harvested from the import data in single
 *         line mode and for which the transfer split could not yet
 *         be created (due to a missing transfer account value). These
 *         parameters will be passed on to the generic importer
 *         which can use this to complete information on the balancing
 *         split for an incomplete transaction
 */
struct DraftTransaction
{
    DraftTransaction (Transaction* tx) : trans(tx) {}
    ~DraftTransaction () { if (trans) { xaccTransDestroy (trans); trans = nullptr; } }
    Transaction* trans;

    boost::optional<GncNumeric> m_price;
    boost::optional<std::string> m_taction;
    boost::optional<std::string> m_tmemo;
    boost::optional<GncNumeric> m_tamount;
    boost::optional<Account*> m_taccount;
    boost::optional<char> m_trec_state;
    boost::optional<GncDate> m_trec_date;

    boost::optional<std::string> void_reason;
};

struct GncPreTrans
{
public:
    GncPreTrans(int date_format, bool multi_split)
        : m_date_format{date_format}, m_multi_split{multi_split} {};

    void set (GncTransPropType prop_type, const std::string& value);
    void set_date_format (int date_format) { m_date_format = date_format ;}
    void set_multi_split (bool multi_split) { m_multi_split = multi_split ;}
    void reset (GncTransPropType prop_type);
    StrVec verify_essentials (void);
    std::shared_ptr<DraftTransaction> create_trans (QofBook* book, gnc_commodity* currency);

    /** Check whether the harvested transaction properties for this instance
     *  match those of another one (the "parent"). Note this function is *not*
     *  symmetrical. This instance can have empty properties and still be considered
     *  part of the parent if the other properties match the parent's.
     *  A fully empty instance will equally be considered part of the parent.
     *
     *  This function is intended to discover multi-split transaction lines in an import
     *  file where the first line defines the transaction (with a first split) and subsequent
     *  lines add splits. These subsequent lines can either have all transaction related
     *  columns be empty or the same as the first line.
     *
     *  @param parent the parent transaction property object to test against
     *  @returns true if this object is considered to be part of the parent, false otherwise.
     */
    bool is_part_of (std::shared_ptr<GncPreTrans> parent);
    boost::optional<std::string> get_void_reason() { return m_void_reason; }
    ErrMap errors();

private:
    int m_date_format;
    bool m_multi_split;
    boost::optional<std::string> m_differ;
    boost::optional<GncDate> m_date;
    boost::optional<std::string> m_num;
    boost::optional<std::string> m_desc;
    boost::optional<std::string> m_notes;
    boost::optional<gnc_commodity*> m_commodity;
    boost::optional<std::string> m_void_reason;
    bool created = false;

    ErrMap m_errors;
};

struct GncPreSplit
{
public:
    GncPreSplit (int date_format, int currency_format) : m_date_format{date_format},
        m_currency_format{currency_format}{};
    void set (GncTransPropType prop_type, const std::string& value);
    void reset (GncTransPropType prop_type);
    void add (GncTransPropType prop_type, const std::string& value);
    void set_date_format (int date_format) { m_date_format = date_format ;}
    void set_currency_format (int currency_format) { m_currency_format = currency_format; }
    StrVec verify_essentials (void);
    void create_split(std::shared_ptr<DraftTransaction> draft_trans);

    Account* get_account () { if (m_account) return *m_account; else return nullptr; }
    void set_account (Account* acct) { if (acct) m_account = acct; else m_account = boost::none; }
    ErrMap errors();

private:
    int m_date_format;
    int m_currency_format;
    boost::optional<std::string> m_action;
    boost::optional<Account*> m_account;
    boost::optional<GncNumeric> m_amount;
    boost::optional<GncNumeric> m_amount_neg;
    boost::optional<GncNumeric> m_price;
    boost::optional<std::string> m_memo;
    boost::optional<char> m_rec_state;
    boost::optional<GncDate> m_rec_date;
    boost::optional<std::string> m_taction;
    boost::optional<Account*> m_taccount;
    boost::optional<GncNumeric> m_tamount;
    boost::optional<GncNumeric> m_tamount_neg;
    boost::optional<std::string> m_tmemo;
    boost::optional<char> m_trec_state;
    boost::optional<GncDate> m_trec_date;
    bool created = false;

    ErrMap m_errors;
};

#endif
