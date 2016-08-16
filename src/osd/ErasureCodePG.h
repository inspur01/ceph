#ifndef CEPH_ERASURECODEPG_H
#define CEPH_ERASURECODEPG_H

#include "ReplicatedPG.h"

class ErasureCodePG;
#ifdef PG_DEBUG_REFS
  typedef TrackedIntPtr<ErasureCodePG> ErasureCodePGRef;
#else
  typedef boost::intrusive_ptr<ErasureCodePG> ErasureCodePGRef;
#endif

class DirectECProcess;

class ErasureCodePG: public ReplicatedPG {
  public:
    friend class DirectECProcess;

    ErasureCodePG(OSDService* o, OSDMapRef curmap, const PGPool& _pool, spg_t p,
        const hobject_t& oid, const hobject_t& ioid);
    ~ErasureCodePG();
    virtual void do_op(OpRequestRef& op);
  private:
    DirectECProcess* decp;

    // 重写ReplicatedPG::execute_ctx()
    // 增加直接纠删的处理流程，即调用DirectECProcess::check_osd_ops()
    // 以及DirectECProcess::do_osd_ops()
    // 目前暂不支持除提交到DirectECProcess的所有其他写操作，所以在流程上仅仅保留了
    // 与读操作相关的代码。
    // 后续根据需要再进行调整。
    void execute_ctx(OpContext* ctx); 

    // 提交repop到DirectECProcess，供execute_ctx()调用的内部方法
    void issue_repop_to_decp(RepGather* repop);

    // 重写ReplicatedPG::do_osd_ops()
    // 去除处理逻辑中关于副本池/纠删池的判断，统一认为是直接纠删池
    // 对读操作，按原有流程处理
    // 对暂不支持的操作，输出错误日志并assert(false)
    int do_osd_ops(OpContext* ctx, vector<OSDOp>& ops);

    // 不再使用repop_queue，只用repop_map实现全部功能

    // 保存已提交/已完成的请求的版本信息
    // 对于已提交而未完成的请求，其version和user_version为空
    // 对于已完成的请求，其version和user_version非空
    // 在new_repop()中，新添加的记录的version和user_version总是为空
    // 在eval_repop()中，如果检查到repop已完成，则设置相应记录的version和user_version
    struct ReqRecord {
      ceph_tid_t rep_tid; // 事务ID，即repop->rep_tid
      eversion_t version; // PG版本暨PGLog版本
      version_t user_version; // 用户版本
    };

    // 从请求ID到请求的版本信息的映射
    // 以便查找是否有重复/回放的客户端请求
    // req_records的最大容量，应与PGLog::IndexedLog的最大容量相当
    // 在裁剪该容器时，只能裁剪那些version非空的记录，也就是已完成的记录
    // 在查询重复/回放的请求时，version非空表示已完成，version为空表示未完成
    // 在on_change()清空repop_map后，要清空req_records中那些version为空的记录
    map<osd_reqid_t, ReqRecord> req_records;

    // 在ReplicatedPG中增加虚方法check_dup_op()，负责检查和处理重复/回放的请求
    // ErasureCodePG将重写该方法
    // 返回值表示是否是重复/回放的请求
    bool check_dup_op(OpRequestRef& op);

    // 重写ReplicatedPG::new_repop()
    // 一是不再使用repop_queue
    // 二是需要添加到req_records
    RepGather* new_repop(OpContext* ctx, ObjectContextRef obc, ceph_tid_t rep_tid);

    // 重写ReplicatedPG::eval_repop()
    // 不再使用repop_queue，在repop完成的时候更新相应的ReqRecord
    void eval_repop(RepGather* repop);

    // 重写ReplicatedPG::apply_and_flush_repops()
    // 该方法主要供ReplicatedPG::on_change()调用
    // 主要重写repop重新入队的方法，包括对repop->waiting_for_ack、repop->waiting_for_disk
    // 的处理，以及req_records的清理等。
    void apply_and_flush_repops(bool requeue);

    // 重写ReplicatedPG::on_local_recover()
    void on_local_recover(
      const hobject_t& oid,
      const object_stat_sum_t& stat_diff,
      const ObjectRecoveryInfo& recovery_info,
      ObjectContextRef obc,
      ObjectStore::Transaction* t
    );
    // wumingqiao added for ErasureCodePG begin
  public:
    void local_recover_done(const hobject_t& hoid, const eversion_t& version);

};

#endif
