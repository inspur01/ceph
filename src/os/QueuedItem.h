#ifndef CEPH_QUEUEDITEM_H
#define CEPH_QUEUEDITEM_H

class QueuedItem {
public:
  enum Type {
    QIT_ASYNC_READ = 0,
    QIT_WRITE,
    QIT_JOURNAL
  };
  
  QueuedItem(Type _type): type(_type), bytes(0) {}
  
  Type get_type() const { return type; }
  uint64_t get_bytes() const { return bytes; }
  
  virtual ~QueuedItem() {}
  
private:
  Type type;
  
protected:
  uint64_t bytes;
};

#endif

