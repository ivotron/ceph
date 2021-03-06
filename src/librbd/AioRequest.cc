// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "common/ceph_context.h"
#include "common/dout.h"
#include "common/Mutex.h"

#include "librbd/AioCompletion.h"
#include "librbd/ImageCtx.h"
#include "librbd/internal.h"

#include "librbd/AioRequest.h"

#define dout_subsys ceph_subsys_rbd
#undef dout_prefix
#define dout_prefix *_dout << "librbd::AioRequest: "

namespace librbd {

  AioRequest::AioRequest() :
    m_ictx(NULL),
    m_object_no(0), m_object_off(0), m_object_len(0),
    m_snap_id(CEPH_NOSNAP), m_completion(NULL), m_parent_completion(NULL),
    m_hide_enoent(false) {}
  AioRequest::AioRequest(ImageCtx *ictx, const std::string &oid,
			 uint64_t objectno, uint64_t off, uint64_t len,
			 librados::snap_t snap_id,
			 Context *completion,
			 bool hide_enoent) {
    m_ictx = ictx;
    m_ioctx.dup(ictx->data_ctx);
    m_ioctx.snap_set_read(snap_id);
    m_oid = oid;
    m_object_no = objectno;
    m_object_off = off;
    m_object_len = len;
    m_snap_id = snap_id;
    m_completion = completion;
    m_parent_completion = NULL;
    m_hide_enoent = hide_enoent;
  }

  AioRequest::~AioRequest() {
    if (m_parent_completion) {
      m_parent_completion->release();
      m_parent_completion = NULL;
    }
  }

  void AioRequest::read_from_parent(vector<pair<uint64_t,uint64_t> >& image_extents)
  {
    assert(!m_parent_completion);
    assert(m_ictx->parent_lock.is_locked());
    m_parent_completion = aio_create_completion_internal(this, rbd_req_cb);
    ldout(m_ictx->cct, 20) << "read_from_parent this = " << this
			   << " parent completion " << m_parent_completion
			   << " extents " << image_extents
			   << dendl;
    aio_read(m_ictx->parent, image_extents, NULL, &m_read_data,
	     m_parent_completion);
  }

  /** read **/

  bool AioRead::should_complete(int r)
  {
    ldout(m_ictx->cct, 20) << "read should_complete: r = " << r << dendl;

    if (!m_tried_parent && r == -ENOENT) {
      Mutex::Locker l(m_ictx->snap_lock);
      Mutex::Locker l2(m_ictx->parent_lock);

      // calculate reverse mapping onto the image
      vector<pair<uint64_t,uint64_t> > image_extents;
      Striper::extent_to_file(m_ictx->cct, &m_ictx->layout,
			    m_object_no, m_object_off, m_object_len,
			    image_extents);

      uint64_t image_overlap = 0;
      r = m_ictx->get_parent_overlap(m_snap_id, &image_overlap);
      if (r < 0) {
	assert(0 == "FIXME");
      }
      uint64_t object_overlap = m_ictx->prune_parent_extents(image_extents, image_overlap);
      if (object_overlap) {
	m_tried_parent = true;
	read_from_parent(image_extents);
	return false;
      }
    }

    return true;
  }

  int AioRead::send() {
    librados::AioCompletion *rados_completion =
      librados::Rados::aio_create_completion(this, rados_req_cb, NULL);
    int r;
    if (m_sparse) {
      r = m_ioctx.aio_sparse_read(m_oid, rados_completion, &m_ext_map,
				  &m_read_data, m_object_len, m_object_off);
    } else {
      r = m_ioctx.aio_read(m_oid, rados_completion, &m_read_data,
			   m_object_len, m_object_off);
    }
    rados_completion->release();
    return r;
  }

  /** read **/

  AbstractWrite::AbstractWrite()
    : m_state(LIBRBD_AIO_WRITE_FINAL),
      m_parent_overlap(0) {}
  AbstractWrite::AbstractWrite(ImageCtx *ictx, const std::string &oid,
			       uint64_t object_no, uint64_t object_off, uint64_t len,
			       vector<pair<uint64_t,uint64_t> >& objectx,
			       uint64_t object_overlap,
			       const ::SnapContext &snapc, librados::snap_t snap_id,
			       Context *completion,
			       bool hide_enoent)
    : AioRequest(ictx, oid, object_no, object_off, len, snap_id, completion, hide_enoent)
  {
    m_state = LIBRBD_AIO_WRITE_FINAL;

    m_object_image_extents = objectx;
    m_parent_overlap = object_overlap;

    // TODO: find a way to make this less stupid
    std::vector<librados::snap_t> snaps;
    for (std::vector<snapid_t>::const_iterator it = snapc.snaps.begin();
	 it != snapc.snaps.end(); ++it) {
      snaps.push_back(it->val);
    }
    m_ioctx.selfmanaged_snap_set_write_ctx(snapc.seq.val, snaps);
  }

  void AbstractWrite::guard_write()
  {
    if (has_parent()) {
      m_state = LIBRBD_AIO_WRITE_CHECK_EXISTS;
      m_read.stat(NULL, NULL, NULL);
    }
    ldout(m_ictx->cct, 20) << __func__ << " has_parent = " << has_parent()
			   << " m_state = " << m_state << " check exists = "
			   << LIBRBD_AIO_WRITE_CHECK_EXISTS << dendl;
      
  }

  bool AbstractWrite::should_complete(int r)
  {
    ldout(m_ictx->cct, 20) << "write " << this << " should_complete: r = "
			   << r << dendl;

    bool finished = true;
    switch (m_state) {
    case LIBRBD_AIO_WRITE_CHECK_EXISTS:
      ldout(m_ictx->cct, 20) << "WRITE_CHECK_EXISTS" << dendl;
      if (r < 0 && r != -ENOENT) {
	ldout(m_ictx->cct, 20) << "error checking for object existence" << dendl;
	break;
      }
      finished = false;
      if (r == -ENOENT) {
	Mutex::Locker l(m_ictx->snap_lock);
	Mutex::Locker l2(m_ictx->parent_lock);

	// copyup the entire object up to the overlap point
	ldout(m_ictx->cct, 20) << "reading from parent " << m_object_image_extents << dendl;
	assert(m_object_image_extents.size());

	m_state = LIBRBD_AIO_WRITE_COPYUP;
	read_from_parent(m_object_image_extents);
	break;
      }
      ldout(m_ictx->cct, 20) << "no need to read from parent" << dendl;
      m_state = LIBRBD_AIO_WRITE_FINAL;
      send();
      break;
    case LIBRBD_AIO_WRITE_COPYUP:
      ldout(m_ictx->cct, 20) << "WRITE_COPYUP" << dendl;
      m_state = LIBRBD_AIO_WRITE_FINAL;
      if (r < 0)
	return should_complete(r);
      send_copyup();
      finished = false;
      break;
    case LIBRBD_AIO_WRITE_FINAL:
      ldout(m_ictx->cct, 20) << "WRITE_FINAL" << dendl;
      // nothing to do
      break;
    default:
      lderr(m_ictx->cct) << "invalid request state: " << m_state << dendl;
      assert(0);
    }

    return finished;
  }

  int AbstractWrite::send() {
    librados::AioCompletion *rados_completion =
      librados::Rados::aio_create_completion(this, NULL, rados_req_cb);
    int r;
    if (m_state == LIBRBD_AIO_WRITE_CHECK_EXISTS) {
      assert(m_read.size());
      r = m_ioctx.aio_operate(m_oid, rados_completion, &m_read, &m_read_data);
    } else {
      assert(m_write.size());
      r = m_ioctx.aio_operate(m_oid, rados_completion, &m_write);
    }
    rados_completion->release();
    return r;
  }

  void AbstractWrite::send_copyup() {
    m_copyup.exec("rbd", "copyup", m_read_data);
    add_copyup_ops();

    librados::AioCompletion *rados_completion =
      librados::Rados::aio_create_completion(this, NULL, rados_req_cb);
    m_ictx->md_ctx.aio_operate(m_oid, rados_completion, &m_copyup);
    rados_completion->release();
  }
}
