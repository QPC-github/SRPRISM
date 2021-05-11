/*  $Id: search.cpp 591182 2019-08-12 16:55:27Z morgulis $
 * ===========================================================================
 *
 *                            PUBLIC DOMAIN NOTICE
 *               National Center for Biotechnology Information
 *
 *  This software/database is a "United States Government Work" under the
 *  terms of the United States Copyright Act.  It was written as part of
 *  the author's official duties as a United States Government employee and
 *  thus cannot be copyrighted.  This software/database is freely available
 *  to the public for use. The National Library of Medicine and the U.S.
 *  Government have not placed any restriction on its use or reproduction.
 *
 *  Although all reasonable efforts have been taken to ensure the accuracy
 *  and reliability of the software and data, the NLM and the U.S.
 *  Government do not and cannot warrant the performance or results that
 *  may be obtained by using this software or data. The NLM and the U.S.
 *  Government disclaim all warranties, express or implied, including
 *  warranties of performance, merchantability or fitness for any particular
 *  purpose.
 *
 *  Please cite the author in any work or product based on this material.
 *
 * ===========================================================================
 *
 * Authors:  Aleksandr Morgulis
 *
 * File Description: representation of a search task
 *
 */

#include <ncbi_pch.hpp>

#include "../common/def.h"

#include <atomic>
#include <thread>
#include <cassert>
#include <string>
#include <vector>
#include <algorithm>

#include "../common/util.hpp"
#include "../common/trace.hpp"
#include "../seq/seqinput_factory.hpp"
#include "../seq/seqinput.hpp"
#include "srprismdef.hpp"
#include "out_sam.hpp"
#include "search.hpp"

START_STD_SCOPES
START_NS( srprism )

USE_NS( common )
USE_NS( seq )

//------------------------------------------------------------------------------
const char * STAT_N_ALIGNS         = "n_aligns";
const char * STAT_N_UALIGNS        = "n_unidir_aligns";
const char * STAT_N_FILTER         = "n_filter";
const char * STAT_N_CANDIDATES     = "n_candidates";
const char * STAT_N_INPLACE        = "n_inplace";
const char * STAT_N_INPLACE_ALIGNS = "n_inplace_align";

//------------------------------------------------------------------------------
S_IPAM ParseResConfStr( std::string rcstr )
{
    static const size_t RESCONF_STR_LEN = 4ULL;

    static const T_IPAM IPAM_INIT_TABLE[RESCONF_STR_LEN][MAX_IPAM_IDX + 1] = 
    {
        { 4, 2, 1, 8 },
        { 8, 1, 8, 1 },
        { 1, 8, 4, 2 },
        { 2, 4, 2, 4 }
    };

    S_IPAM result; std::fill( result.data, result.data + MAX_IPAM_IDX + 1, 0 );

    if( rcstr == "illumina" || rcstr == "454" ) rcstr = "0100";
    else if( rcstr == "solid" ) rcstr = "0010";

    if( rcstr.size() != RESCONF_STR_LEN ) {
        M_TRACE( common::CTracer::ERROR_LVL,
                 "result configuration string must be " << RESCONF_STR_LEN );
        return result;
    }

    for( size_t i( 0 ); i < RESCONF_STR_LEN; ++i ) {
        if( rcstr[i] == '1' ) {
            for( size_t j( 0 ); j <= MAX_IPAM_IDX; ++j ) {
                result.data[j] |= IPAM_INIT_TABLE[i][j];
            }
        }
        else if( rcstr[i] != '0' ) {
            M_TRACE( common::CTracer::ERROR_LVL,
                     "result configuration string must consist of characters "
                     "'0' or '1'" );
            std::fill( result.data, result.data + MAX_IPAM_IDX + 1, 0 );
            return result;
        }
    }

    return result;
}

//------------------------------------------------------------------------------
CSearch::CSearch( const SOptions & options )
{
    global_stats_.NewCounter( STAT_N_ALIGNS );
    global_stats_.NewCounter( STAT_N_UALIGNS );
    global_stats_.NewCounter( STAT_N_FILTER );
    global_stats_.NewCounter( STAT_N_CANDIDATES );
    global_stats_.NewCounter( STAT_N_INPLACE );
    global_stats_.NewCounter( STAT_N_INPLACE_ALIGNS );
    batch_init_data_.search_stats = &global_stats_;
    
    Validate( options );
    mem_mgr_p_.reset( new CMemoryManager( MEGABYTE*options.mem_limit ) );
    input_          = options.input;
    input_fmt_      = options.input_fmt;
    extra_tags_     = options.extra_tags;
    use_sids_       = options.use_sids;
    force_paired_   = options.force_paired;
    force_unpaired_ = options.force_unpaired;
    strict_batch_   = options.strict_batch;
    start_batch_    = options.start_batch - 1;
    end_batch_      = options.end_batch - 1;
    batch_limit_    = options.batch_limit;
    if( force_paired_ ) batch_limit_ *= 2;
    input_c_        = options.input_compression;
    skip_unmapped_  = options.skip_unmapped;
    use_qids_       = options.use_qids;

    if( options.sa_start < 0 ) { 
        std::string rs( options.resconf_str );
        std::swap( rs[0], rs[2] );
        std::swap( rs[1], rs[3] );
        batch_init_data_.ipam_vec = ParseResConfStr( rs );
        batch_init_data_.resconf_str = rs;
    }
    else {
        batch_init_data_.ipam_vec = ParseResConfStr( options.resconf_str );
        batch_init_data_.resconf_str = options.resconf_str;
    }

    {
        bool valid( false );

        for( size_t i( 0 ); i < MAX_IPAM_IDX + 1; ++i ) {
            if( batch_init_data_.ipam_vec.data[i] != 0 ) {
                valid = true;
                break;
            }
        }

        if( !valid ) {
            M_THROW( CException, VALIDATE, "wrong strand configuration" );
        }
    }

    batch_init_data_.index_basename = options.index_basename;
    batch_init_data_.tmpdir         = options.tmpdir;
    batch_init_data_.res_limit      = options.res_limit;
    batch_init_data_.pair_distance  = options.pair_distance;
    batch_init_data_.pair_fuzz      = options.pair_fuzz;
    batch_init_data_.max_qlen       = options.max_qlen;
    batch_init_data_.n_err          = options.n_err;
    batch_init_data_.use_qids       = options.use_qids;
    batch_init_data_.use_sids       = options.use_sids;
    batch_init_data_.n_threads      = options.n_threads;
    batch_init_data_.sa_start       = options.sa_start;
    batch_init_data_.sa_end         = options.sa_end;
    batch_init_data_.paired_log     = options.paired_log;
    batch_init_data_.use_fixed_hc   = options.use_fixed_hc;
    batch_init_data_.fixed_hc       = options.fixed_hc;
    batch_init_data_.search_mode    = options.search_mode;
    batch_init_data_.hist_fname     = options.hist_fname;
    batch_init_data_.discover_sep   = options.discover_sep;
    batch_init_data_.discover_sep_stop = options.discover_sep_stop;
    batch_init_data_.randomize      = options.randomize;
    batch_init_data_.random_seed    = options.random_seed;

    batch_init_data_.repeat_threshold = options.repeat_threshold;

    // static const size_t TMP_RES_BUF_SIZE = 1024*1024ULL;
    static const size_t TMP_RES_BUF_SIZE = CBatch::TMP_RES_BUF_SIZE;

    batch_init_data_.u_tmp_res_buf_size = TMP_RES_BUF_SIZE;
    batch_init_data_.p_tmp_res_buf_size = TMP_RES_BUF_SIZE;
    batch_init_data_.u_tmp_res_buf = 
    batch_init_data_.p_tmp_res_buf = nullptr;

    if( options.n_threads == 1 )
    {
        char * t( (char *)mem_mgr_p_->Allocate( TMP_RES_BUF_SIZE ) );
        batch_init_data_.u_tmp_res_buf = t;
        t = (char *)mem_mgr_p_->Allocate( TMP_RES_BUF_SIZE );
        batch_init_data_.p_tmp_res_buf = t;
    }

    seqstore_p_.reset( 
            new CSeqStore( options.index_basename, *mem_mgr_p_.get() ) );
    sidmap_p_.reset( 0 );

    if( options.use_sids ) {
        sidmap_p_.reset( 
                new CSIdMap( options.index_basename, *mem_mgr_p_.get() ) );
    }

    // batch_init_data_.mem_mgr_p = mem_mgr_p_.get();
    batch_init_data_.mem_mgr_p = mem_mgr_p_;
    batch_init_data_.seqstore_p = seqstore_p_.get();

    tmp_store_p_.reset( new CTmpStore( options.tmpdir ) );
    // batch_init_data_.out_tmp_store_p = tmp_store_p_.get();
    /*
    out_p_.reset( new COutSAM(
                options.output, options.input,
                options.input_fmt, options.extra_tags,
                options.cmdline, options.sam_header,
                options.input_compression,
                options.skip_unmapped,
                options.force_paired, options.force_unpaired,
                !options.use_qids,
                ( options.search_mode == SSearchMode::DEFAULT ||
                  options.search_mode == SSearchMode::SUM_ERR ),
                seqstore_p_.get(), sidmap_p_.get() ) );

    batch_init_data_.out_p = out_p_.get();
    */
    out_p_.reset( new COutSAM_Collator(
        options.output, options.cmdline,
        seqstore_p_.get(), sidmap_p_.get(), options.sam_header ) );
}

//------------------------------------------------------------------------------
CSearch::~CSearch()
{
}

//------------------------------------------------------------------------------
void CSearch::Validate( const SOptions & opt ) const
{
    if( opt.search_mode != SSearchMode::DEFAULT && 
        opt.search_mode != SSearchMode::SUM_ERR &&
        opt.search_mode != SSearchMode::PARTIAL &&
        opt.search_mode != SSearchMode::BOUND_ERR ) {
        M_THROW( CException, VALIDATE, "unknown search mode" );
    }

    if( opt.mem_limit == 0 ) {
        M_THROW( CException, VALIDATE,
                 "the value of memory limit must be positive" <<
                 " (given " << opt.mem_limit << ")" );
    }

    if( opt.batch_limit == 0 ) {
        M_THROW( CException, VALIDATE,
                 "the value of batch size limit must be positive" <<
                 " (given " << opt.batch_limit << ")" );
    }

    if( opt.start_batch < 1 ) {
        M_THROW( CException, VALIDATE,
                 "the value of start batch must be positive" <<
                 " (given " << opt.start_batch << ")" );
    }

    if( opt.end_batch < opt.start_batch ) {
        M_THROW( CException, VALIDATE,
                 "the value of end batch must be greater or equal to "
                 "start batch (given start batch " << opt.start_batch <<
                 ", end batch " << opt.end_batch << ")" );
    }

    if( opt.res_limit < MIN_RES_LIMIT || opt.res_limit > MAX_RES_LIMIT ) {
        M_THROW( CException, VALIDATE,
                 "invalid value of max number of results reported: " << 
                 opt.res_limit <<
                 "; value must be between " << MIN_RES_LIMIT <<
                 " and " << MAX_RES_LIMIT );
    }

    if( opt.pair_distance == 0 ) {
        M_THROW( CException, VALIDATE,
                 "the value of pair distance must be positive" <<
                 " (given " << opt.pair_distance << ")" );
    }

    if( opt.pair_distance < opt.pair_fuzz ) {
        M_THROW( CException, VALIDATE,
                 "the value of pair distance fuzz (given " << 
                 opt.pair_fuzz << ") must be at most the value of " <<
                 "pair distance (given " << opt.pair_distance << ")" );
    }

    if( opt.pair_fuzz > MAX_PAIR_FUZZ ) {
        M_THROW( CException, VALIDATE,
                 "the value of pair distance fuzz (given " << opt.pair_fuzz <<
                 ") must be at most " <<
                 MAX_PAIR_FUZZ );
    }

    if( opt.max_qlen < MIN_QLEN ) {
        M_THROW( CException, VALIDATE,
                "the value of max query length (given " << opt.max_qlen <<
                ") must be at least " << MIN_QLEN );
    }

    if( opt.max_qlen > MAX_QLEN ) {
        M_THROW( CException, VALIDATE,
                "the value of max query length (given " << opt.max_qlen <<
                ") must be at most " << MAX_QLEN );
    }

    if( opt.n_err > MAX_N_ERR ) {
        M_THROW( CException, VALIDATE,
                 "invalid requested number of errors " << (int)opt.n_err <<
                 "; the value must be at most " << (int)MAX_N_ERR );
    }

    if( opt.force_paired && opt.force_unpaired ) {
        M_THROW( CException, VALIDATE,
                 "both forced paired and unpaired search requested" );
    }

    if( opt.sa_start == 0 ) {
        M_THROW( CException, VALIDATE, "sa-start value can not have value 0" );
    }

    if( opt.sa_start > 0 ) {
        if( opt.sa_end < opt.sa_start ) {
            M_THROW( CException, VALIDATE,
                     "sa-start value must be less or equal to sa-end value; "
                     "given sa-start: " << opt.sa_start << 
                     "; given sa-end: " << opt.sa_end  );
        }
    }

    if( opt.sa_start < 0 ) {
        if( opt.sa_end > opt.sa_start ) {
            M_THROW( CException, VALIDATE,
                     "sa-start value must be greater or equal to sa-end value; "
                     "given sa-start: " << opt.sa_start << 
                     "; given sa-end: " << opt.sa_end  );
        }
    }
}

//------------------------------------------------------------------------------
namespace
{
    struct thread_info
    {
        std::atomic< bool > done;
        std::shared_ptr< std::thread > th;
    };
}

//------------------------------------------------------------------------------
void CSearch::Run_priv(void)
{
    static char const * TMP_SAM_OUT = "sam-out-";

    int request_cols( 0 );
    if( force_unpaired_ ) request_cols = 1;
    if( force_paired_ ) request_cols = 2;

    if( request_cols == 0 ) {
        M_THROW( CException, INPUT,
                 "neither paired nor unpaired search is requested" );
    }

    std::auto_ptr< CSeqInput > in( CSeqInputFactory::MakeSeqInput( 
                input_fmt_, input_, request_cols, input_c_ ) );

    if( force_paired_ && in->NCols() != 2 ) {
        M_THROW( CException, INPUT,
                 "paired search is requested but input is not paired" );
    }

    if( force_unpaired_ && in->NCols() != 1 ) {
        M_THROW( CException, INPUT,
                 "unpaired search is requested but input is not unpaired" );
    }

    batch_init_data_.paired = (in->NCols() == 2);
    TQueryOrdId start_qid( 0 ), batch_start_qid( 0 );
    Uint4 batch_num( 0 ), batch_oid( 0 );

    static char const * OUT_FNAME_PFX( "outsam-" );
    std::map< Uint4, thread_info > threads;
    Uint4 batch_out( 0 );

    /*
    COutSAM_Collator out(
        options.output, options.cmdline,
        seqstore_p_.get(), sidmap_p_.get(), options.sam_header );
    */

    while( !in->Done() && batch_num <= end_batch_ ) {
        batch_init_data_.batch_limit = 
            batch_limit_ - (start_qid - batch_start_qid);
        std::shared_ptr< CBatch > batch( std::make_shared< CBatch >( batch_init_data_, *in, start_qid, batch_oid ) );

        // setup local batch output
        {
            std::string in_fname_pfx( CQueryStore::INPUT_DUMP_NAME );
            in_fname_pfx += std::to_string( batch_oid );
            std::string out_fname_pfx( OUT_FNAME_PFX );
            out_fname_pfx += std::to_string( batch_oid );
            auto out_fname( tmp_store_p_->Register( out_fname_pfx ) );
            batch->SetBatchOutput( new COutSAM(
                // options.output, options.input,
                out_fname, in_fname_pfx,
                // options.input_fmt, options.extra_tags,
                "fasta", extra_tags_,
                // options.cmdline, options.sam_header,
                "", false,
                // options.input_compression,
                CFileBase::COMPRESSION_NONE,
                // options.skip_unmapped,
                skip_unmapped_,
                // options.force_paired, options.force_unpaired,
                force_paired_, force_unpaired_,
                // !options.use_qids,
                !use_qids_,
                /*
                ( options.search_mode == SSearchMode::DEFAULT ||
                  options.search_mode == SSearchMode::SUM_ERR ),
                */
                ( batch_init_data_.search_mode == SSearchMode::DEFAULT ||
                  batch_init_data_.search_mode == SSearchMode::SUM_ERR ),
                seqstore_p_.get(), sidmap_p_.get() ) );
        }

        if( batch_num >= start_batch_ && batch_num <= end_batch_ ) {
            if( batch_init_data_.n_threads == 1 )
            {
                /*
                 *  cont can be false only in case read insert size
                 *  discovery is requested, which forces single
                 *  threadedness
                 */
                bool cont;

                switch( batch_init_data_.paired ) {
                    case true:  cont = batch->Run< true >(); break;
                    case false: cont = batch->Run< false >(); break;
                }

                // append batch results to the output
                //
                {
                    std::string out_fname_pfx( OUT_FNAME_PFX );
                    out_fname_pfx += std::to_string( batch_oid );
                    auto out_fname( tmp_store_p_->Register( out_fname_pfx ) );
                    out_p_->Append( out_fname );
                }

                // stop if needed
                //
                if( !cont ) break;
            }
            else
            {
                // poll until a thread slot is free
                //
                while( true )
                {
                    for( auto ti( threads.begin() ); ti != threads.end(); )
                    {
                        if( ti->second.done )
                        {
                            ti->second.th->join();
                            ti->second.th.reset();
                            ti = threads.erase( ti );
                        }
                        else ++ti;
                    }

                    if( threads.size() == batch_init_data_.n_threads )
                    {
                        std::this_thread::sleep_for(
                            std::chrono::seconds( 1 ) );
                    }
                    else break;
                }

                // start current batch in the new thread
                //
                std::shared_ptr< std::thread > th;

                if( batch_init_data_.paired )
                {
                    th.reset( new std::thread(
                        []( std::shared_ptr< CBatch > batch )
                        { batch->Run< true >(); },
                        batch ) );
                }
                else
                {
                    th.reset( new std::thread(
                        []( std::shared_ptr< CBatch > batch )
                        { batch->Run< false >(); },
                        batch ) );
                }

                threads[batch_oid].done = false;
                threads[batch_oid].th = th;

                // check if we have some output to report
                //
                for( ; batch_out < batch_oid; ++batch_out )
                {
                    if( threads.count( batch_out ) == 0 )
                    {
                        std::string out_fname_pfx( OUT_FNAME_PFX );
                        out_fname_pfx += std::to_string( batch_out );
                        auto out_fname( tmp_store_p_->Register( out_fname_pfx ) );
                        out_p_->Append( out_fname );
                    }
                    else break;
                }
            }
        }
        else M_TRACE( CTracer::INFO_LVL, "skipping batch " << 1 + batch_num );

        ++batch_oid;
        start_qid = batch->EndQId();

        if( !strict_batch_ || start_qid - batch_start_qid == batch_limit_ ) {
            batch_start_qid = start_qid;
            ++batch_num;
        }
    }

    while( !threads.empty() )
    {
        auto ti( threads.begin() );
        ti->second.th->join();
        ti->second.th.reset();
        threads.erase( ti );
    }

    // report the rest of the output
    //
    for( ; batch_out < batch_oid; ++batch_out )
    {
        if( threads.count( batch_out ) == 0 )
        {
            std::string out_fname_pfx( OUT_FNAME_PFX );
            out_fname_pfx += std::to_string( batch_out );
            auto out_fname( tmp_store_p_->Register( out_fname_pfx ) );
            out_p_->Append( out_fname );
        }
        else break;
    }
}

//------------------------------------------------------------------------------
void CSearch::Run(void) { Run_priv(); }

END_NS( srprism )
END_STD_SCOPES

