#include <amax.recover/amax.recover.hpp>

#include<math.hpp>

#include <utils.hpp>

static constexpr eosio::name active_permission{"active"_n};
static constexpr symbol   APL_SYMBOL          = symbol(symbol_code("APL"), 4);
static constexpr eosio::name MT_BANK{"amax.token"_n};

#define ALLOT_APPLE(farm_contract, lease_id, to, quantity, memo) \
    {   aplink::farm::allot_action(farm_contract, { {_self, active_perm} }).send( \
            lease_id, to, quantity, memo );}

namespace amax {

using namespace std;

#define CHECKC(exp, code, msg) \
   { if (!(exp)) eosio::check(false, string("[[") + to_string((int)code) + string("]] ") + msg); }


   inline int64_t get_precision(const symbol &s) {
      int64_t digit = s.precision();
      CHECK(digit >= 0 && digit <= 18, "precision digit " + std::to_string(digit) + " should be in range[0,18]");
      return calc_precision(digit);
   }

   void amax_recover::init( const uint8_t& score_limit, const name default_audit_contract) {
      require_auth( _self );
      _gstate.score_limit                 = score_limit;
      _gstate.default_audit_contract      = default_audit_contract;
   }

   void amax_recover::bindaccount ( const name& admin, const name& account ) {

      _check_action_auth(admin, ActionPermType::BINDACCOUNT);

      check(is_account(account), "account invalid: " + account.to_string());

      account_audit_t::idx accountaudits(_self, _self.value);
      auto audit_ptr     = accountaudits.find(account.value);
      CHECKC( audit_ptr == accountaudits.end(), err::RECORD_EXISTING, "account already exist. ");
      auto now           = current_time_point();
      accountaudits.emplace( _self, [&]( auto& row ) {
         row.account 		                                    = account;
         row.audit_contracts[_gstate.default_audit_contract]   = ContractAuditStatus::REGISTED;
         row.threshold                                         = _gstate.score_limit;
         row.created_at                                        = now;
      });   
   }

   void amax_recover::addauth( const name& account, const name& contract ) {
      // CHECKC( has_auth(account) , err::NO_AUTH, "no auth for operate" )

      account_audit_t::idx account_audits(_self, _self.value);
      auto audit_ptr     = account_audits.find(account.value);
      CHECKC( audit_ptr == account_audits.end(), err::RECORD_EXISTING, "account already exist. ");
      auto now           = current_time_point();

      // CHECKC(audit_ptr->audit_contracts.count(contract) == 0, err::RECORD_EXISTING, "contract already existed") 

      account_audits.emplace( _self, [&]( auto& row ) {
         row.audit_contracts[contract]  = ContractAuditStatus::OPTIONAL;
         row.created_at                 = now;
      });   
   }


   void amax_recover::checkauth( const name& contract, const name& account ) {
      CHECKC (get_first_receiver() == contract, err::NO_AUTH, "receiver must contract" )

      account_audit_t::idx accountaudits(_self, _self.value);
      auto audit_ptr     = accountaudits.find(account.value);
      CHECKC( audit_ptr == accountaudits.end(), err::RECORD_EXISTING, "account already exist. ");
      auto now           = current_time_point();

      CHECKC(audit_ptr->audit_contracts.count(contract) != 0, err::RECORD_NOT_FOUND, "contract not existed")
   }


   void amax_recover::createorder(const name&      admin,
                        const name&                account,
                        const recover_target_type& recover_target,
                        const bool&                manual_check_required) {
      _check_action_auth(admin, ActionPermType::CREATEORDER);

      account_audit_t::idx accountaudits(_self, _self.value);
      auto audit_ptr     = accountaudits.find(account.value);
      CHECKC( audit_ptr != accountaudits.end(), err::RECORD_NOT_FOUND, "account not exist. ");

      recoverorder_t::idx_t orders( _self, _self.value );
      auto account_index 			      = orders.get_index<"accountidx"_n>();
      auto order_itr 			         = account_index.find( account.value );
      CHECKC( order_itr == account_index.end(), err::RECORD_EXISTING, "order already existed. ");

      auto duration_second    = order_expiry_duration;
      if (manual_check_required) {
         duration_second      = manual_order_expiry_duration;
      }

      _gstate.last_order_id ++;
      auto order_id           = _gstate.last_order_id; 
      auto now                = current_time_point();

      orders.emplace( _self, [&]( auto& row ) {
         row.id 					      = order_id;
         row.account 			      = account;
         row.recover_type           = UpdateActionType::PUBKEY;
         row.recover_target         = recover_target;
         row.pay_status             = PayStatus::NOPAY;
         row.created_at             = now;
         row.expired_at             = now + eosio::seconds(duration_second);
      });
   
   }

   void amax_recover::setscore( const name& contract, const uint64_t& order_id, const uint8_t& score) {
      CHECKC (get_first_receiver() == contract, err::NO_AUTH, "receiver must contract" )

      auditscore_t::idx_t auditscores(_self, _self.value);
      auto auditscore_ptr     = auditscores.find(contract.value);
      CHECKC( auditscore_ptr != auditscores.end(), err::RECORD_NOT_FOUND, "contract not exist. ");
      CHECKC( auditscore_ptr->status == ContractStatus::RUNNING, err::STATUS_ERROR, "contract isn't running");

      recoverorder_t::idx_t orders(_self, _self.value);
      auto order_ptr     = orders.find(order_id);
      CHECKC( order_ptr != orders.end(), err::RECORD_NOT_FOUND, "order not found. ");
      auto answer_score_limit = _get_audit_score(contract);
      CHECKC(answer_score_limit >= score, err::PARAM_ERROR, "scores exceed limit")
      CHECKC(order_ptr->expired_at > current_time_point(), err::TIME_EXPIRED, "order already time expired")

      orders.modify(*order_ptr, _self, [&]( auto& row ) {
         row.scores[contract]    = score;
         row.updated_at          = current_time_point();
      });
   }

   void amax_recover::chkmanual( const name& admin, const uint64_t& order_id, const bool& passed) {
      _check_action_auth(admin, ActionPermType::CHKMANUAL);

      recoverorder_t::idx_t orders(_self, _self.value);
      auto order_ptr     = orders.find(order_id);
      CHECKC( order_ptr != orders.end(), err::RECORD_NOT_FOUND, "order not found. ");
      CHECKC(order_ptr->expired_at > current_time_point(), err::TIME_EXPIRED, "order already time expired")

      name manual_check_result       = ManualCheckStatus::FAILURE;
      if (passed) manual_check_result   = ManualCheckStatus::SUCCESS;
      auto now                = current_time_point();
      orders.modify(*order_ptr, _self, [&]( auto& row ) {
         row.manual_check_result    = manual_check_result;
         row.manual_checker         = admin;
         row.updated_at             = now;
      });
   }

   void amax_recover::closeorder( const name& submitter, const uint64_t& order_id) {
      CHECKC( has_auth(submitter) , err::NO_AUTH, "no auth for operate" )
      recoverorder_t::idx_t orders(_self, _self.value);
      auto order_ptr     = orders.find(order_id);
      CHECKC( order_ptr != orders.end(), err::RECORD_NOT_FOUND, "order not found. "); 
      CHECKC(order_ptr->expired_at > current_time_point(), err::TIME_EXPIRED, "order already time expired")

      auto total_score = 0;
      for (auto& [key, value]: order_ptr->scores) {
            total_score += value;
      }


      CHECKC( total_score > _gstate.score_limit, err::SCORE_NOT_ENOUGH, "score not enough" );

      CHECKC((!order_ptr->manual_check_required) || 
         (order_ptr->manual_check_required && order_ptr->manual_check_result == ManualCheckStatus::SUCCESS ) ,
         err::STATUS_ERROR, "not ready for closing");

      _update_authex(order_ptr->account, std::get<eosio::public_key>(order_ptr->recover_target));

      account_audit_t::idx accountaudits(_self, _self.value);
      auto audit_ptr     = accountaudits.find(order_ptr->account.value);
      CHECKC( audit_ptr != accountaudits.end(), err::RECORD_NOT_FOUND, "order not exist. ");

      accountaudits.modify( *audit_ptr, _self, [&]( auto& row ) {
         row.recovered_at  = current_time_point();
      });   

      orders.erase(order_ptr);
   }

   void amax_recover::delorder( const name& submitter, const uint64_t& order_id) {
      CHECKC( has_auth(submitter) , err::NO_AUTH, "no auth for operate" )
      recoverorder_t::idx_t orders(_self, _self.value);
      auto order_ptr     = orders.find(order_id);
      CHECKC( order_ptr != orders.end(), err::RECORD_NOT_FOUND, "order not found. "); 
      auto total_score = 0;
      CHECKC(order_ptr->expired_at < current_time_point(), err::STATUS_ERROR, "order has not expired")
      orders.erase(order_ptr);
   
   }

   void amax_recover::setauditor( const name& account, const set<name>& actions ) {
      CHECKC(has_auth(_self),  err::NO_AUTH, "no auth for operate");      

      auditor_t::idx_t auditors(_self, _self.value);
      auto auditor_ptr = auditors.find(account.value);

      if( auditor_ptr != auditors.end() ) {
         auditors.modify(*auditor_ptr, _self, [&]( auto& row ) {
            row.actions      = actions;
         });   
      } else {
         auditors.emplace(_self, [&]( auto& row ) {
            row.account      = account;
            row.actions      = actions;
         });
      }
   }
      
   void amax_recover::delauditor(  const name& account ) {
      CHECKC(has_auth(_self),  err::NO_AUTH, "no auth for operate");      

      auditor_t::idx_t auditors(_self, _self.value);
      auto auditor_ptr     = auditors.find(account.value);

      CHECKC( auditor_ptr != auditors.end(), err::RECORD_EXISTING, "auditor not exist. ");
      auditors.erase(auditor_ptr);
   }

   void amax_recover::addcontract(  const name&    contract, 
                                 const asset&   cost, 
                                 const string&  title, 
                                 const string&  desc, 
                                 const string&  url,
                                 const uint8_t& score,
                                 const name     status ) {
      CHECKC(has_auth(_self),  err::NO_AUTH, "no auth for operate"); 


      auditscore_t::idx_t auditscores(_self, _self.value);
      auto auditscore_ptr     = auditscores.find(contract.value);
      if( auditscore_ptr != auditscores.end() ) {
         auditscores.modify(*auditscore_ptr, _self, [&]( auto& row ) {
            row.cost          = cost;
            row.title         = title;
            row.desc          = desc;
            row.url           = url;
            row.cost          = cost;
            row.status        = status;
         });   
      } else {
         auditscores.emplace(_self, [&]( auto& row ) {
            row.contract      = contract;
            row.cost          = cost;
            row.title         = title;
            row.desc          = desc;
            row.url           = url;
            row.cost          = cost;
         });
      }
   }

   void amax_recover::delcontract(  const name& account ) {
      CHECKC(has_auth(_self),  err::NO_AUTH, "no auth for operate");      

      auditscore_t::idx_t auditscores(_self, _self.value);
      auto auditscore_ptr     = auditscores.find(account.value);

      CHECKC( auditscore_ptr != auditscores.end(), err::RECORD_NOT_FOUND, "auditscore not exist. ");
      auditscores.erase(auditscore_ptr);
   }

   void amax_recover::_check_action_auth(const name& admin, const name& action_type) {
      if(has_auth(_self)) 
         return;
      auditor_t::idx_t auditors(_self, _self.value);
      auto auditor_ptr     = auditors.find(admin.value);
      CHECKC( auditor_ptr != auditors.end(), err::RECORD_NOT_FOUND, "auditor not exist. ");
      CHECKC( auditor_ptr->actions.count(action_type), err::NO_AUTH, "no auth for operate ");
      CHECKC(has_auth(admin),  err::NO_AUTH, "no auth for operate");      
   }

   int8_t amax_recover::_get_audit_score( const name& contract) {
      auditscore_t::idx_t auditorscores(_self, _self.value);
      auto auditorscore_ptr     = auditorscores.find(contract.value);
      CHECKC( auditorscore_ptr != auditorscores.end(), err::RECORD_NOT_FOUND, "auditorscore not exist. ");
      return auditorscore_ptr->score;
   }

   void amax_recover::_update_authex( const name& account,
                                  const eosio::public_key& pubkey ) {
      print( "_update_authex: ", account, "\n");
      eosiosystem::authority auth = { 1, {{pubkey, 1}}, {}, {} };
      eosiosystem::system_contract::updateauth_action act(amax_account, { {account, owner} });
      act.send( account, "active", "owner"_n, auth);

   }

}//namespace amax