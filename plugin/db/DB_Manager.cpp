/*
 * DB_Manager.cpp
 *
 *  Created on: Nov 9, 2016
 *      Author: zhangyalei
 */

#include "Data_Manager.h"
#include "Struct_Manager.h"
#include "Node_Manager.h"
#include "DB_Manager.h"

DB_Manager::DB_Manager(void) : 
	idx_value_map_(get_hash_table_size(64)), 
	save_idx_tick_(Time_Value::gettimeofday().sec()),
	db_id_(0),
	struct_name_(""),
	session_map_(get_hash_table_size(10000)), 
	data_node_idx_(0), 
	data_connector_size_(0) { }

DB_Manager::~DB_Manager(void) { }

DB_Manager *DB_Manager::instance_;

DB_Manager *DB_Manager::instance(void) {
	if (instance_ == 0)
		instance_ = new DB_Manager;
	return instance_;
}

int DB_Manager::init(const Node_Info &node_info) { 
	node_info_ = node_info; 
	data_node_idx_ = node_info_.node_id;

	Xml xml;
	int ret = xml.load_xml("./config/node/node_conf.xml");
	if(ret < 0) {
		LOG_FATAL("load config:node_conf.xml abort");
		return -1;
	}

	//连接mysql
	TiXmlNode* node = xml.get_root_node("node_list");
	if(node) {
		TiXmlNode* child_node = xml.enter_node(node, "node_list");
		if(child_node) {
			XML_LOOP_BEGIN(child_node)
				if(xml.get_attr_int(child_node, "type") == node_info_.node_type) {
					TiXmlNode* sub_node = xml.enter_node(child_node, "node");
					if(sub_node) {
						XML_LOOP_BEGIN(sub_node)
							std::string key = xml.get_key(sub_node);
							if(key == "mysql" || key == "mongodb") {
								int db_id = xml.get_attr_int(sub_node, "db_id");
								std::string ip = xml.get_attr_str(sub_node, "ip");
								int port = xml.get_attr_int(sub_node, "port");
								std::string user = xml.get_attr_str(sub_node, "user");
								std::string password = xml.get_attr_str(sub_node, "password");
								std::string pool = xml.get_attr_str(sub_node, "pool");
								DATA_MANAGER->connect_to_db(db_id, ip, port, user, password, pool);
							}
						XML_LOOP_END(sub_node)
					}
					break;
				}
			XML_LOOP_END(child_node)
		}
	}

	//初始化idx表信息
	TiXmlNode* idx_node = xml.get_root_node("idx");
	if(idx_node) {
		db_id_ = xml.get_attr_int(idx_node, "db_id");
		struct_name_ = xml.get_attr_str(idx_node, "struct_name");
		Bit_Buffer buffer;
		ret = DATA_MANAGER->load_db_data(db_id_, struct_name_, 0, buffer);
		if(ret >= 0) {
			uint length = buffer.read_uint(16);
			for(uint i = 0; i < length; ++i) {
				std::string type = "";
				buffer.read_str(type);
				int value = buffer.read_int(32);
				idx_value_map_.insert(std::make_pair(type, value));
			}
		}
	}
	return 0;
}

void DB_Manager::run_handler(void) {
	process_list();
}

int DB_Manager::process_list(void) {
	Msg_Head msg_head;
	Bit_Buffer bit_buffer;
	Byte_Buffer *buffer = nullptr;
	while (true) {
		//此处加锁是为了防止本线程其他地方同时等待条件变量成立
		notify_lock_.lock();

		//put wait in while cause there can be spurious wake up (due to signal/ENITR)
		while (buffer_list_.empty() && tick_list_.empty()) {
			notify_lock_.wait();
		}

		buffer = buffer_list_.pop_front();
		if(buffer != nullptr) {
			int session_size = sid_set_.size();
			if(node_info_.endpoint_gid == GID_DATA_SERVER && session_size >= node_info_.max_session_count) {
				//计算需要创建log_connector的数量，如果大于0，就创建
				int fork_size = session_size / node_info_.max_session_count - data_connector_size_;
				if(fork_size > 0 && data_fork_list_.count(data_node_idx_) <= 0) {
					std::string node_name = "data_connector";
					NODE_MANAGER->fork_process(node_info_.node_type, ++data_node_idx_, 2, node_name);
					data_fork_list_.insert(data_node_idx_);
				}
			}

			msg_head.reset();
			buffer->read_head(msg_head);
			int eid = msg_head.eid;
			int cid = msg_head.cid;
			if(msg_head.msg_id == SYNC_NODE_INFO || msg_head.msg_id == SYNC_DB_RET_CODE || msg_head.msg_id == SYNC_RES_GENERATE_ID || msg_head.msg_id == SYNC_RES_SELECT_DB_DATA
				|| ((msg_head.msg_id == SYNC_SAVE_DB_DATA || msg_head.msg_id == SYNC_SAVE_RUNTIME_DATA) && msg_head.msg_type == DATA_MSG)) {
				//同步节点信息是子进程启动后发过来的
				if(msg_head.msg_id == SYNC_NODE_INFO) {
					data_connector_list_.push_back(cid); 
					data_connector_size_++;
					//插入正在fork过程中的node_id
					Node_Info node_info;
					node_info.deserialize(bit_buffer);
					data_fork_list_.erase(node_info.node_id);
				}
				else {
					//这些消息是data_connector发回来经过data_server转发到逻辑进程
					Session_Map::iterator iter = session_map_.find(msg_head.sid);
					if(iter != session_map_.end()) {
						msg_head.cid = iter->second;
						msg_head.msg_type = NODE_MSG;
						NODE_MANAGER->send_msg(msg_head, buffer->get_read_ptr(), buffer->readable_bytes());
					}
					else {
						LOG_ERROR("find session error, eid:%d, cid:%d, msg_id:%d, msg_type:%d, sid:%d",
							msg_head.eid, msg_head.cid, msg_head.msg_id, msg_head.msg_type, msg_head.sid);
					}
				}
			}
			else {
				//如果当前进程处理的sid已超上限，而且有已经有创建好的data_connector,sid不在本进程处理列表，就转发到connector
				if(node_info_.endpoint_gid == GID_DATA_SERVER && data_connector_size_ > 0 
					&& session_size >= node_info_.max_session_count && sid_set_.count(msg_head.sid) <= 0) {
					//保存转发到connector进程的seesion信息
					session_map_.insert(std::make_pair(msg_head.sid, cid));
					int index = random() % (data_connector_size_);
					msg_head.cid = data_connector_list_[index];
					msg_head.msg_type = DATA_MSG;
					NODE_MANAGER->send_msg(msg_head, buffer->get_read_ptr(), buffer->readable_bytes());
				}
				else {
					//保存sid在本进程
					sid_set_.insert(msg_head.sid);
					if(node_info_.endpoint_gid == GID_DATA_CONNECTOR) {
						msg_head.msg_type = DATA_MSG;
					}
					bit_buffer.reset();
					bit_buffer.set_ary(buffer->get_read_ptr(), buffer->readable_bytes());

					switch(msg_head.msg_id) {
					case SYNC_SELECT_DB_DATA:
						select_db_data(msg_head, bit_buffer);
						break;
					case SYNC_GENERATE_ID:
						generate_id(msg_head, bit_buffer);
						break;
					case SYNC_LOAD_DB_DATA:
						load_db_data(msg_head, bit_buffer);
						break;
					case SYNC_SAVE_DB_DATA:
						save_db_data(msg_head, bit_buffer);
						break;
					case SYNC_DELETE_DB_DATA:
						delete_db_data(msg_head, bit_buffer);
						break;
					case SYNC_LOAD_RUNTIME_DATA:
						load_runtime_data(msg_head, bit_buffer);
						break;
					case SYNC_SAVE_RUNTIME_DATA:
						save_runtime_data(msg_head, bit_buffer);
						break;
					case SYNC_DELETE_RUNTIME_DATA:
						delete_runtime_data(msg_head, bit_buffer);
						break;
					default:
						break;
					}
				}
			}

			//回收buffer
			NODE_MANAGER->push_buffer(eid, cid, buffer);
		}

		if(!tick_list_.empty()) {
			int tick_time = tick_list_.pop_front();
			if(tick_time > 0) {
				tick(tick_time);
			}
		}

		//操作完成解锁条件变量
		notify_lock_.unlock();
	}
	return 0;
}

int DB_Manager::tick(int tick_time) {
	if(tick_time - save_idx_tick_ >= 30) {
		save_idx_tick_ = tick_time;
		DATA_MANAGER->print_cache_data();
		
		Bit_Buffer buffer;
		buffer.write_uint(idx_value_map_.size(), 16);
		for(Idx_Value_Map::iterator iter = idx_value_map_.begin(); iter != idx_value_map_.end(); ++iter) {
			buffer.write_str(iter->first.c_str());
			buffer.write_int(iter->second, 32);
		}
		DATA_MANAGER->save_db_data(SAVE_DB_AND_CACHE, true, db_id_, struct_name_, buffer);
	}
	return 0;
}

void DB_Manager::select_db_data(Msg_Head &msg_head, Bit_Buffer &buffer) {
	uint db_id = buffer.read_uint(16);
	std::string struct_name = "";
	std::string condition_name = "";
	std::string condition_value = "";
	std::string query_name = "";
	std::string query_type = "";
	buffer.read_str(struct_name);
	buffer.read_str(condition_name);
	buffer.read_str(condition_value);
	buffer.read_str(query_name);
	buffer.read_str(query_type);
	uint data_type = buffer.read_uint(8);

	uint8_t opt_msg_id = msg_head.msg_id;
	msg_head.msg_id = SYNC_RES_SELECT_DB_DATA;
	Bit_Buffer bit_buffer;
	bit_buffer.write_uint(db_id, 16);
	bit_buffer.write_str(struct_name.c_str());
	bit_buffer.write_str(query_name.c_str());
	bit_buffer.write_uint(data_type, 8);
	int ret = DATA_MANAGER->select_db_data(db_id, struct_name, condition_name, condition_value, query_name, query_type, bit_buffer);
	if(ret < 0) {
		msg_head.msg_id = SYNC_DB_RET_CODE;
		bit_buffer.reset();
		build_ret_buffer(bit_buffer, opt_msg_id, ret, struct_name, query_name, 0);
	}
	NODE_MANAGER->send_msg(msg_head, bit_buffer.data(), bit_buffer.get_byte_size());
}

void DB_Manager::generate_id(Msg_Head &msg_head, Bit_Buffer &buffer) {
	int idx = 1;
	std::string type = "";
	buffer.read_str(type);
	Idx_Value_Map::iterator iter = idx_value_map_.find(type);
	if(iter != idx_value_map_.end()) {
		idx = ++(iter->second);
	}
	else {
		idx_value_map_.insert(std::make_pair(type, idx));
	}

	int64_t id = make_id(STRUCT_MANAGER->agent_num(), STRUCT_MANAGER->server_num(), idx);
	msg_head.msg_id = SYNC_RES_GENERATE_ID;
	Bit_Buffer bit_buffer;
	bit_buffer.write_str(type.c_str());
	bit_buffer.write_int64(id);
	NODE_MANAGER->send_msg(msg_head, bit_buffer.data(), bit_buffer.get_byte_size());
}

void DB_Manager::load_db_data(Msg_Head &msg_head, Bit_Buffer &buffer) {
	uint db_id = buffer.read_uint(16);
	std::string struct_name = "";
	buffer.read_str(struct_name);
	int64_t key_index = buffer.read_int64();
	uint data_type = buffer.read_uint(8);

	uint8_t opt_msg_id = msg_head.msg_id;
	msg_head.msg_id = SYNC_SAVE_DB_DATA;
	Bit_Buffer bit_buffer;
	bit_buffer.write_uint(0, 2);
	bit_buffer.write_bool(false);
	bit_buffer.write_uint(db_id, 16);
	bit_buffer.write_str(struct_name.c_str());
	bit_buffer.write_uint(data_type, 8);
	int ret = DATA_MANAGER->load_db_data(db_id, struct_name, key_index, bit_buffer);
	if(ret < 0) {
		msg_head.msg_id = SYNC_DB_RET_CODE;
		bit_buffer.reset();
		build_ret_buffer(bit_buffer, opt_msg_id, ret, struct_name, "", key_index);
	}
	NODE_MANAGER->send_msg(msg_head, bit_buffer.data(), bit_buffer.get_byte_size());
}

void DB_Manager::save_db_data(Msg_Head &msg_head, Bit_Buffer &buffer) {
	uint save_type = buffer.read_uint(2);
	bool vector_data = buffer.read_bool();
	uint db_id = buffer.read_uint(16);
	std::string struct_name = "";
	buffer.read_str(struct_name);
	/*uint data_type*/buffer.read_uint(8);
	int ret = DATA_MANAGER->save_db_data(save_type, vector_data, db_id, struct_name, buffer);
	if(ret < 0 || (ret >=0 && save_type == SAVE_DB_CLEAR_CACHE)) {
		uint8_t opt_msg_id = msg_head.msg_id;
		msg_head.msg_id = SYNC_DB_RET_CODE;
		Bit_Buffer bit_buffer;
		build_ret_buffer(bit_buffer, opt_msg_id, ret, struct_name, "", 0);
		NODE_MANAGER->send_msg(msg_head, bit_buffer.data(), bit_buffer.get_byte_size());

		//玩家下线保存，清除session信息
		if(ret >= 0) {
			sid_set_.erase(msg_head.sid);
		}
	}
}

void DB_Manager::delete_db_data(Msg_Head &msg_head, Bit_Buffer &buffer) {
	uint db_id = buffer.read_uint(16);
	std::string struct_name = "";
	buffer.read_str(struct_name);
	int ret = DATA_MANAGER->delete_db_data(db_id, struct_name, buffer);
	if(ret < 0) {
		uint8_t opt_msg_id = msg_head.msg_id;
		msg_head.msg_id = SYNC_DB_RET_CODE;
		Bit_Buffer bit_buffer;
		build_ret_buffer(bit_buffer, opt_msg_id, ret, struct_name, "", 0);
		NODE_MANAGER->send_msg(msg_head, bit_buffer.data(), bit_buffer.get_byte_size());
	}
}

void DB_Manager::load_runtime_data(Msg_Head &msg_head, Bit_Buffer &buffer) {
	std::string struct_name = "";
	buffer.read_str(struct_name);
	int64_t key_index = buffer.read_int64();
	uint data_type = buffer.read_uint(8);

	uint8_t opt_msg_id = msg_head.msg_id;
	msg_head.msg_id = SYNC_SAVE_RUNTIME_DATA;
	Bit_Buffer bit_buffer;
	bit_buffer.write_str(struct_name.c_str());
	bit_buffer.write_int64(key_index);
	bit_buffer.write_uint(data_type, 8);
	int ret = DATA_MANAGER->load_runtime_data(struct_name, key_index, bit_buffer);
	if(ret < 0) {
		msg_head.msg_id = SYNC_DB_RET_CODE;
		bit_buffer.reset();
		build_ret_buffer(bit_buffer, opt_msg_id, ret, struct_name, "", key_index);
	}
	NODE_MANAGER->send_msg(msg_head, bit_buffer.data(), bit_buffer.get_byte_size());
}

void DB_Manager::save_runtime_data(Msg_Head &msg_head, Bit_Buffer &buffer) {
	std::string struct_name = "";
	buffer.read_str(struct_name);
	int64_t key_index = buffer.read_int64();
	/*uint data_type*/buffer.read_uint(8);
	int ret = DATA_MANAGER->save_runtime_data(struct_name, key_index, buffer);
	if(ret < 0) {
		uint8_t opt_msg_id = msg_head.msg_id;
		msg_head.msg_id = SYNC_DB_RET_CODE;
		Bit_Buffer bit_buffer;
		build_ret_buffer(bit_buffer, opt_msg_id, ret, struct_name, "", key_index);
		NODE_MANAGER->send_msg(msg_head, bit_buffer.data(), bit_buffer.get_byte_size());
	}
}

void DB_Manager::delete_runtime_data(Msg_Head &msg_head, Bit_Buffer &buffer) {
	std::string struct_name = "";
	buffer.read_str(struct_name);
	int64_t key_index = buffer.read_int64();
	int ret = DATA_MANAGER->delete_runtime_data(struct_name, key_index);
	if(ret < 0) {
		uint8_t opt_msg_id = msg_head.msg_id;
		msg_head.msg_id = SYNC_DB_RET_CODE;
		Bit_Buffer bit_buffer;
		build_ret_buffer(bit_buffer, opt_msg_id, ret, struct_name, "", key_index);
		NODE_MANAGER->send_msg(msg_head, bit_buffer.data(), bit_buffer.get_byte_size());
	}
}