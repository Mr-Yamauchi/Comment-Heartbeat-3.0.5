
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */


#include <lha_internal.h>

#include "ccm.h"
#include "ccmmsg.h"
#include "ccmmisc.h"
#include <clplumbing/cl_plugin.h>
#include <clplumbing/cl_quorum.h>
#include <clplumbing/cl_tiebreaker.h>
#include <clplumbing/cl_misc.h>


extern state_msg_handler_t	state_msg_handler[];

struct ha_msg * ccm_readmsg(ccm_info_t *info, ll_cluster_t *hb);
static GList* quorum_list = NULL;
/* 他ノードのHeartbeat上でCCMプロセスがHeartbeatのクライアントとして、接続・切断した時に通知処理 */
static struct ha_msg*
ccm_handle_hbapiclstat(ccm_info_t *info,  
		       const char *orig, 
		       const char *status)
{
	int 		index;
	enum ccm_state 	state = info->state;
	
	if(state == CCM_STATE_NONE ||
		state == CCM_STATE_VERSION_REQUEST) {
		//CCM_STATE_NONE,CCM_STATE_VERSION_REQUEST中は処理しない
		return NULL;
	}

	assert(status);
	if(strncmp(status, JOINSTATUS, 5) == 0) {
		//他ノードのHeartbeat上でCCMプロセスがHeartbeatのクライアントとして、接続した時は、debugメッセージでRETURN
		ccm_debug2(LOG_DEBUG,
		       "ccm on %s started", orig);
		return NULL;
	}

	if(!orig){
		return NULL;
	}
	
	index = llm_get_index(&info->llm, orig);
	if(index == -1) {
		//認識していない他ノードなら無視する
		return NULL;
	}
	//ここまでくるのは切断メッセージのみで、認識しているノードの場合
	//切断したノード情報のCCM_TYPE_LEAVEメッセージを生成して返す。
	return(ccm_create_leave_msg(info, index));
}

/* 
 * The callback function which is called when the status
 * of a node changes. 
 */

static int
nodelist_update(ll_cluster_t* hb, ccm_info_t* info, 
		const char *id, const char *status)
{
	llm_info_t *llm;
	char oldstatus[STATUSSIZE];
	/* update the low level membership of the node
	 * if the status moves from active to dead and if the member
	 * is already part of the ccm, then we have to mimic a
	 * leave message for us 
	 */
	ccm_debug2(LOG_DEBUG, 
	       "nodelist update: Node %s now has status %s", 
	       id,  status);
	llm = &info->llm;
	
	if (llm_status_update(llm, id, status,oldstatus) != HA_OK) {
		ccm_log(LOG_ERR, "%s: updating status for node %s failed",
		       __FUNCTION__, id);
		return HA_FAIL;
	}
	
	if (STRNCMP_CONST(status, DEADSTATUS) == 0){
		
		if(node_is_member(info, id)){
			/* インデックスをleave情報にキャッシュする */
			leave_cache(llm_get_index(llm, id));
		}
		
	}
	
	if ( strncmp(llm_get_mynodename(llm), id,NODEIDSIZE ) == 0){
		return HA_OK;
	}
	
	ccm_debug(LOG_DEBUG, "status of node %s: %s -> %s"
	,	id, oldstatus, status);
	
	if ( part_of_cluster(info->state)
	     && ( STRNCMP_CONST(oldstatus, DEADSTATUS) == 0
		  && STRNCMP_CONST(status, DEADSTATUS) != 0)){
		ccm_send_state_info(hb, info, id);
	}
	
	return HA_OK;
}

/* 制御処理(Heartbeatメッセージ) */
int
ccm_control_process(ccm_info_t *info, ll_cluster_t * hb)
{
	struct ha_msg*	msg;
	struct ha_msg*	newmsg;
	const char*	type;
	int		ccm_msg_type;
	const char*	orig=NULL;
	const char*	status=NULL;
	llm_info_t*	llm= &info->llm;
	const char*	mynode = llm_get_mynodename(llm);
	const char*	numnodes;
	int		numnodes_val;
	
 repeat:
	/* read the next available message */
	/* 受信メッセージがあれば取得する  */
	msg = ccm_readmsg(info, hb); /* this is non-blocking */

	if (msg) {
		/* 受信メッセージ有りの場合、TYPE,送信元、STATUSを取り出す */
		type = ha_msg_value(msg, F_TYPE);
		orig = ha_msg_value(msg, F_ORIG);
		status = ha_msg_value(msg, F_STATUS);
		
		ccm_debug(LOG_DEBUG, "recv msg %s from %s, status:%s"
		,	type, orig, (status ? status : "[null ptr]"));
		ccm_message_debug2(LOG_DEBUG, msg);
		
		if(strcmp(type, T_APICLISTAT) == 0){
			/* TYPEがT_APICLISTATの場合 */
			/* 他ノードのHeartbeat上でCCMプロセスがHeartbeatのクライアントとして、接続・切断した時に通知される */
			/* handle ccm status of on other nodes of the cluster */
		       	if((newmsg = ccm_handle_hbapiclstat(info, orig, 
				status)) == NULL) {
					/* 他ノードのccm接続メッセージの場合は、RETURN */
				ha_msg_del(msg);
				return TRUE;
			}
			/* 切断メッセージの場合は続けて、ccm_handle_hbapiclstat()で生成されたCCM_TYPE_LEAVEメッセージを処理 */
			ha_msg_del(msg);
			msg = newmsg;
		} else if(strcasecmp(type, T_STATUS) == 0){
			//
			//HeartbeatからのSTATUS通信を処理する
			//
			const char* nodetype;
			const char* site;
			int	weight;
			if (llm_is_valid_node(&info->llm, orig)){
				/* 管理情報に存在するノードの場合は、ノード情報を更新する  */
				if (nodelist_update(hb, info,orig, status) != HA_OK){
					ccm_log(LOG_ERR, "%s: updating node status for "
					       "nodelist failed(%s-%s)",
					       __FUNCTION__, orig, status);
					return FALSE;
				}
				/* メッセージを破棄してリターン */
				ha_msg_del(msg);
				return TRUE;
			}
			/* 管理情報に存在しないノードの場合はノードタイプを取得する */
			nodetype = hb->llc_ops->node_type(hb, orig); ★１回目
			if (nodetype == NULL){
				ccm_log(LOG_ERR, "%s: get node %s's type failed",
				       __FUNCTION__, orig);
				return TRUE;
			}
			
			/* ノードタイプがNORMALNODEでない場合は無視する */
			if (STRNCMP_CONST(nodetype, NORMALNODE) !=0 ){
				/* メッセージ解放が必要な気がするが通常はありえないのだと思われる */
				return TRUE;
			}
			/* 管理情報に存在するノードの場合はノードタイプを取得する */
			nodetype = hb->llc_ops->node_type(hb, orig); ★２回目（この２回目は冗長ではないのか？)
			/* サイト、ウエイトを取得する */
			site = hb->llc_ops->node_site(hb, orig);
			weight = hb->llc_ops->node_weight(hb, orig);
			
			/* 管理情報に追加する */
			if (llm_add(llm, orig, status, mynode, site, weight) != HA_OK){
				ccm_log(LOG_ERR, "%s: adding node(%s) to llm failed",
				       __FUNCTION__,orig);
				return FALSE;
			}
			
			ccm_debug2(LOG_INFO, "after adding node %s", orig);
			llm_display(llm);
			/* CCM_STATE_JOINING状態へ遷移する */
			jump_to_joining_state(hb, info, msg);
			
		} else if (strcasecmp(type, T_DELNODE) ==0){
			/* T_DELNODEメッセージの場合 */
			const char* node = ha_msg_value(msg, F_NODE);
			if (node == NULL){
				ccm_log(LOG_ERR, "%s: field node not found",
				       __FUNCTION__);
				return FALSE;
			}
			/* 管理情報からの削除する */
			if (llm_del(llm, node) != HA_OK){
				ccm_log(LOG_ERR, "%s: deleting node %s failed",
				       __FUNCTION__, node);
				return FALSE;
			}
			/* CCM_STATE_JOINING状態へ遷移する */
			jump_to_joining_state(hb, info, msg);
			
		}

	} else {
		/* メッセージが受信出来ない場合、CCM_TYPE_TIMEOUTメッセージを生成する */
		/* １秒周期のタイマーからの呼び出しの場合 */
		msg = timeout_msg_mod(info);
	}

	/* メッセージタイプを取り出す */
	type = ha_msg_value(msg, F_TYPE);
	ccm_msg_type = ccm_string2type(type);
	
	if (ccm_msg_type < 0){
		goto out;
	}
	/* メッセージからノード数を取り出す */
	numnodes = ha_msg_value(msg, F_NUMNODES);
	if(numnodes != NULL){
		numnodes_val = atoi(numnodes);
		if (numnodes_val != info->llm.nodecount){
			/* 取り出したノード数と管理情報のノード数が違う場合は、エラー処理する */
			ccm_log(LOG_ERR
			,	"%s: Node count from node %s does not agree"
			": local count=%d, count in message=%d"
			,	__FUNCTION__
			,	orig, info->llm.nodecount, numnodes_val);
			ccm_log(LOG_ERR, "Please make sure ha.cf files on all"
			" nodes have same nodes list or add \"autojoin any\" "
			"to ha.cf");
			ccm_log(LOG_INFO, "%s",	"If this problem persists"
			", check the heartbeat 'hostcache' files"
			" in the cluster to look for problems.");
			exit(1);
		}
	}
	//状態処理ハンドラを実行する
	/*
	state_msg_handler_t	state_msg_handler[]={
	ccm_state_none,
	ccm_state_version_request,
	ccm_state_joining,  						//CCM_STATE_JOINING状態でのメッセージ受信処理
	ccm_state_sent_memlistreq,					//CCM_STATE_SENT_MEMLISTREQ状態でのメッセージ受信処理
	ccm_state_memlist_res,						//CCM_STATE_MEMLIST_RES状態でのメッセージ受信処理
	ccm_state_joined, 							//
	ccm_state_wait_for_mem_list,
	ccm_state_wait_for_change,
	ccm_state_new_node_wait_for_mem_list,
	};
	*/
	state_msg_handler[info->state](ccm_msg_type, msg, hb, info);
	

 out:
	if(ccm_msg_type != CCM_TYPE_TIMEOUT) {
		/* メッセージタイプがCCM_TYPE_TIMEOUTでない場合は、メッセージエリアを解放する */
		ha_msg_del(msg);
	}
	
	/* If there is another message in the channel,
	 * process it now. 
	 */
	/* まだメッセージがある場合はメッセージを処理する */
	if (hb->llc_ops->msgready(hb))	
		goto repeat;
	
	return TRUE;
}
#define QUORUM_S "HA_quorum"
#define TIEBREAKER_S "HA_tiebreaker"
gboolean
ccm_calculate_quorum(ccm_info_t* info)
{
	struct hb_quorum_fns* funcs = NULL;
	const char* quorum_env = NULL;
	char* quorum_str = NULL;
	char* end = NULL;
	char* begin = NULL;
	GList* cur = NULL;
	int rc;
	
	
	if (quorum_list == NULL){
		quorum_env = cl_get_env(QUORUM_S);
		if (quorum_env == NULL){
			ccm_debug(LOG_DEBUG, "No quorum selected,"
			       "using default quorum plugin(majority:twonodes)");
			quorum_str = strdup("majority:twonodes");
		}
		else {
			quorum_str = strdup(quorum_env);
		}
		
		begin = quorum_str;
		while (begin != NULL) {
			end = strchr(begin, ':');
			if (end != NULL) {
				*end = 0;
			}
			funcs = cl_load_plugin("quorum", begin);
			if (funcs == NULL){
				ccm_log(LOG_ERR, "%s: loading plugin %s failed",
				       __FUNCTION__, begin);
			}
			else {
				funcs->init(ccm_on_quorum_changed
				,	info->cluster, info->quorum_server);
				quorum_list = g_list_append(quorum_list, funcs);
			}
			begin = (end == NULL)? NULL:end+1;
		}
		free(quorum_str);
	}
		
	cur = g_list_first(quorum_list);
	while (cur != NULL) {
		int mem_weight = 0;
		int total_weight = 0;
		int i, node;
		
		for (i=0; i<info->memcount; i++) {
			node = info->ccm_member[i];
			mem_weight+=info->llm.nodes[node].weight;
		}
		for (i=0; i<info->llm.nodecount; i++) {
			total_weight+=info->llm.nodes[i].weight;
		}
		funcs = (struct hb_quorum_fns*)cur->data;
	        rc = funcs->getquorum(info->cluster, info->memcount, mem_weight
	        ,		info->llm.nodecount, total_weight);
	        
		if (rc == QUORUM_YES){
			return TRUE;
		}
		else if (rc == QUORUM_NO){
			return FALSE;
		}
		cur = g_list_next(cur);
	}
	ccm_debug(LOG_ERR, "all quorum plugins can't make a decision! "
			"assume lost quorum");
	
	return FALSE;
	
}
gboolean
ccm_stop_query_quorum(void)
{
	struct hb_quorum_fns* funcs = NULL;
	GList* cur = NULL;
	if (quorum_list == NULL){
		return TRUE;
	}
	
	cur = g_list_first(quorum_list);
	while (cur != NULL) {
		funcs = (struct hb_quorum_fns*)cur->data;
		funcs->stop();
		cur = g_list_next(cur);
	}
	return FALSE;
}
