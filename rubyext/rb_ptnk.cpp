#include <ruby.h>
#include <ruby/encoding.h>

// for htonl
#include <netinet/in.h>
#include <arpa/inet.h>

#include <ptnk.h>

extern "C"
{

typedef VALUE (*ruby_method_t)(...);
void Init_ptnk();

VALUE RBM_ptnk = Qnil;
VALUE RBK_db = Qnil;
VALUE RBK_dbtx = Qnil;
VALUE RBK_table = Qnil;
VALUE RBK_cur = Qnil;

inline
ptnk::BufferCRef
val2cref(VALUE val, VALUE* tmp)
{
	if(PTNK_UNLIKELY(NIL_P(val)))
	{
		return ptnk::BufferCRef::NULL_VAL;	
	}
	else
	{
		*tmp = StringValue(val);	
		return ptnk::BufferCRef(RSTRING_PTR(*tmp), RSTRING_LEN(*tmp));
	}
}

inline
VALUE
cref2val(const ptnk::BufferCRef& buf)
{
	if(PTNK_UNLIKELY(buf.isNull()))
	{
		return Qnil;	
	}
	else if(PTNK_LIKELY(buf.isValid()))
	{
		return rb_str_new(buf.get(), buf.size());
	}
	else
	{
		// value not found
		return Qfalse;	
	}
}

struct rb_ptnk_db
{
	ptnk::DB* impl;
};

void
rb_ptnk_db_free(rb_ptnk_db* db)
{
	if(db->impl)
	{
		delete db->impl;
	}
	db->impl = NULL;
}

VALUE db_close(VALUE self);

VALUE
db_new(int argc, VALUE* argv, VALUE klass)
{
	VALUE ret;

	rb_ptnk_db* db;
	ret = Data_Make_Struct(klass, rb_ptnk_db, NULL, rb_ptnk_db_free, db);

	const char* filename = "";
	ptnk_opts_t opts = ptnk::ODEFAULT;
	int mode = 0644;

	switch(argc)
	{
	case 3:
		mode = FIX2INT(argv[2]);
		/* FALL THROUGH */
	
	case 2:
		opts = FIX2INT(argv[1]);
		/* FALL THROUGH */

	case 1:
		filename = StringValueCStr(argv[0]);
	}

	db->impl = new ptnk::DB(filename, opts, mode);

	if(rb_block_given_p())
	{
		return rb_ensure((ruby_method_t)rb_yield, ret, (ruby_method_t)db_close, ret);
	}

	return ret;
}

VALUE
db_close(VALUE self)
{
	rb_ptnk_db* db;
	Data_Get_Struct(self, rb_ptnk_db, db);
	if(db->impl)
	{
		delete db->impl;
		db->impl = NULL;	
	}

	return Qnil;
}
#define GET_DB_WRAP(VDB) \
	rb_ptnk_db* db; \
	Data_Get_Struct(VDB, rb_ptnk_db, db); \
	if(! db->impl) rb_raise(rb_eRuntimeError, "db is closed");

#define COMMON_CATCH_BLOCKS \
	catch(std::exception& e) \
	{ \
		rb_raise(rb_eRuntimeError, e.what().c_str()); \
		return 0; \
	} \
	catch(...) \
	{ \
		rb_raise(rb_eRuntimeError, "unknown C++ exception caught"); \
		return 0; \
	}

VALUE
db_get(VALUE self, VALUE key)
{
	GET_DB_WRAP(self);

	VALUE ret = rb_str_buf_new(2048);

	VALUE tmpKey;
	ssize_t sz = db->impl->get(val2cref(key, &tmpKey), ptnk::BufferRef(RSTRING(ret)->as.heap.ptr, 2048));
	if(sz >= 0)
	{
		RSTRING(ret)->as.heap.len = sz;
		return ret;
	}
	else if(sz == ptnk::BufferCRef::NULL_TAG)
	{
		return Qnil;	
	}
	else
	{
		// value not found
		return Qfalse;	
	}
}

VALUE
db_put(int argc, VALUE* argv, VALUE self)
{
	if(PTNK_UNLIKELY(argc < 2))
	{
		rb_raise(rb_eArgError, "only 1 arg given");	
	}

	GET_DB_WRAP(self);

	ptnk::put_mode_t pm = ptnk::PUT_UPDATE;
	if(argc > 3)
	{
		pm = static_cast<ptnk::put_mode_t>(FIX2INT(argv[2]));
	}

	VALUE tmpKey, tmpVal;
	db->impl->put(val2cref(argv[0], &tmpKey), val2cref(argv[1], &tmpVal), pm);
	
	return argv[1];
}

struct rb_ptnk_dbtx
{
	VALUE vdb;
	ptnk::DB::Tx* impl;
};

void
rb_ptnk_dbtx_mark(rb_ptnk_dbtx* dbtx)
{
	// mark DB ruby obj to avoid db being gc-ed while tx is active
	rb_gc_mark(dbtx->vdb);
}

void
rb_ptnk_dbtx_free(rb_ptnk_dbtx* dbtx)
{
	if(dbtx->impl)
	{
		delete dbtx->impl;
	}
	dbtx->impl = NULL;
}

VALUE
dbtx_new(VALUE klass, VALUE vdb)
{
	VALUE ret;

	if(! rb_obj_is_kind_of(vdb, RBK_db))
	{
		rb_raise(rb_eArgError, "non Ptnk::DB passed to Ptnk::DB::Tx.new");	
	}
	GET_DB_WRAP(vdb);

	rb_ptnk_dbtx* dbtx;
	ret = Data_Make_Struct(klass, rb_ptnk_dbtx, rb_ptnk_dbtx_mark, rb_ptnk_dbtx_free, dbtx);

	dbtx->impl = db->impl->newTransaction();
	dbtx->vdb = vdb;

	return ret;
}
#define GET_DBTX_WRAP \
	rb_ptnk_dbtx* dbtx; \
	Data_Get_Struct(self, rb_ptnk_dbtx, dbtx); \
	do { \
		if(! dbtx->impl) rb_raise(rb_eRuntimeError, "tx already committed/aborted"); \
		GET_DB_WRAP(dbtx->vdb); /* assert db open */ \
	} while(0)

VALUE
dbtx_try_commit(int argc, VALUE* argv, VALUE self)
{
	GET_DBTX_WRAP;

	// parse args
	bool bCommit = true;
	if(argc > 2) rb_raise(rb_eArgError, "too many args");
	if(argc == 1)
	{
		bCommit = RTEST(argv);
	}

	// try commit
	bool bSuccess = false;
	if(bCommit)
	{
		bSuccess = dbtx->impl->tryCommit();
	}
	delete dbtx->impl;
	dbtx->impl = NULL;

	return bSuccess ? Qtrue : Qfalse;
}

VALUE
dbtx_abort(VALUE self)
{
	GET_DBTX_WRAP;

	delete dbtx->impl;
	dbtx->impl = NULL;
}

VALUE
dbtx_table_get_names(VALUE self)
{
	GET_DBTX_WRAP;

	VALUE ret, vname;
	ret = rb_ary_new();

	for(int i = 0;; ++ i)
	{
		ptnk::Buffer name;
		dbtx->impl->tableGetName(i, &name);

		if(! name.isValid()) break;

		vname = rb_str_new(name.get(), name.valsize());

		rb_ary_push(ret, vname);
	}

	return ret;
}

struct rb_ptnk_table
{
	ptnk::TableOffCache* impl;
};

void
rb_ptnk_table_free(rb_ptnk_table* table)
{
	if(table->impl)
	{
		delete table->impl;
	}
	table->impl = NULL;
}

VALUE
wrap_table(ptnk::TableOffCache* t)
{
	rb_ptnk_table* table;
	VALUE ret = Data_Make_Struct(RBK_table, rb_ptnk_table, 0, rb_ptnk_table_free, table);

	table->impl = t;

	return ret;
}

VALUE
table_new(VALUE klass, VALUE vtableid)
{
	VALUE tmp;

	return wrap_table(new ptnk::TableOffCache(val2cref(vtableid, &tmp)));
}
#define GET_TABLE_WRAP(VAL) \
	if(! rb_obj_is_kind_of(VAL, RBK_table)) rb_raise(rb_eArgError, "arg must be Ptnk::Table"); \
	rb_ptnk_table* table; \
	Data_Get_Struct(VAL, rb_ptnk_table, table); \
	if(! table->impl) rb_raise(rb_eArgError, "table already freed");

VALUE
dbtx_table_create(VALUE self, VALUE vtableid)
{
	GET_DBTX_WRAP;

	VALUE tmp;
	ptnk::BufferCRef tableid = val2cref(vtableid, &tmp);
	dbtx->impl->tableCreate(tableid);
	
	return wrap_table(new ptnk::TableOffCache(tableid));
}

VALUE
dbtx_table_drop(VALUE self, VALUE vtableid)
{
	GET_DBTX_WRAP;

	VALUE tmp;
	dbtx->impl->tableDrop(val2cref(vtableid, &tmp));
	
	return Qnil;
}

VALUE
dbtx_get(int argc, VALUE* argv, VALUE self)
{
	VALUE vtable = Qnil, vkey;
	switch(argc)
	{
	case 2:
		vtable = argv[0]; argv++;
		/* fall through */

	case 1:
		vkey = argv[0];
		break;

	default:
		rb_raise(rb_eArgError, "invalid number of args");
	}

	GET_DBTX_WRAP;
	
	ptnk::Buffer val;
	VALUE tmp;
	if(! NIL_P(vtable))
	{
		// record get from specified table

		GET_TABLE_WRAP(vtable);
		
		dbtx->impl->get(table->impl, val2cref(vkey, &tmp), &val);
	}
	else
	{
		// record get from default table

		dbtx->impl->get(val2cref(vkey, &tmp), &val);
	}

	return cref2val(val.rref());
}

VALUE
dbtx_put(int argc, VALUE* argv, VALUE self)
{
	ptnk::put_mode_t pm;

	VALUE vtable = Qnil, vkey, vval;
	switch(argc)
	{
	case 4:
		vtable = argv[0]; argv++;
		/* fall through */

	case 3:
		vkey = argv[0];
		vval = argv[1];
		pm = static_cast<ptnk::put_mode_t>(FIX2INT(argv[2]));
		break;

	default:
		rb_raise(rb_eArgError, "invalid number of args");
	}

	GET_DBTX_WRAP;
	
	VALUE tmp, tmp2;
	if(! NIL_P(vtable))
	{
		// record put to specified table

		GET_TABLE_WRAP(vtable);
		
		dbtx->impl->put(table->impl, val2cref(vkey, &tmp), val2cref(vval, &tmp2), pm);
	}
	else
	{
		// record put to default table

		dbtx->impl->put(val2cref(vkey, &tmp), val2cref(vval, &tmp2), pm);
	}

	return Qnil;
}

struct rb_ptnk_cur
{
	VALUE vtx;
	VALUE vtable;
	ptnk::DB::Tx::cursor_t* impl;
};

void
rb_ptnk_cur_mark(rb_ptnk_cur* cur)
{
	rb_gc_mark(cur->vtx);
	rb_gc_mark(cur->vtable);
}

void
rb_ptnk_cur_free(rb_ptnk_cur* cur)
{
	ptnk::DB::Tx::curClose(cur->impl);
	cur->impl = NULL;
}

VALUE
wrap_cur(ptnk::DB::Tx::cursor_t* c, VALUE vtx, VALUE vtable)
{
	VALUE ret;

	rb_ptnk_cur* cur;
	ret = Data_Make_Struct(RBK_cur, rb_ptnk_cur, rb_ptnk_cur_mark, rb_ptnk_cur_free, cur);
	cur->vtx = vtx;
	cur->vtable = vtable;
	cur->impl = c;

	return ret;
}
#define GET_CUR_WRAP(VAL) \
	rb_ptnk_cur* cur; \
	Data_Get_Struct(VAL, rb_ptnk_cur, cur); \
	if(! cur->impl) rb_raise(rb_eArgError, "cursor already freed"); \
	rb_ptnk_dbtx* dbtx; \
	Data_Get_Struct(cur->vtx, rb_ptnk_dbtx, dbtx); \
	do { if(! dbtx->impl) rb_raise(rb_eRuntimeError, "tx already committed/aborted"); } while(0)

VALUE
cur_get(VALUE self)
{
	GET_CUR_WRAP(self);

	VALUE ret = rb_ary_new();

	ptnk::Buffer k, v;
	dbtx->impl->curGet(&k, &v, cur->impl);

	rb_ary_push(ret, cref2val(k.rref()));
	rb_ary_push(ret, cref2val(v.rref()));

	return ret;
}

VALUE
cur_next(VALUE self)
{
	GET_CUR_WRAP(self);

	if(dbtx->impl->curNext(cur->impl))
	{
		return self;	
	}
	else
	{
		return Qnil;
	}
}

VALUE
dbtx_cursor_front(VALUE self, VALUE vtable)
{
	GET_DBTX_WRAP;
	GET_TABLE_WRAP(vtable);

	ptnk::DB::Tx::cursor_t* c = dbtx->impl->curFront(table->impl);
	if(! c)
	{
		return Qnil;
	}

	return wrap_cur(c, self, vtable);
}

VALUE
dbtx_cursor_query(int argc, VALUE* argv, VALUE self)
{
	VALUE tmp;

	ptnk::query_t q;
	switch(argc)
	{
	case 3:
		q.type = static_cast<ptnk::query_type_t>(FIX2INT(argv[2]));
		/* FALL THROUGH */

	case 2:
		q.key = val2cref(argv[1], &tmp);
		break;

	case 1:
	case 0:
		rb_raise(rb_eArgError, "not enough args (accept 2 or 3");

	default:
		rb_raise(rb_eArgError, "too many args");
	}
	VALUE vtable = argv[0];
	GET_TABLE_WRAP(vtable);

	GET_DBTX_WRAP;

	ptnk::DB::Tx::cursor_t* c = dbtx->impl->curQuery(table->impl, q);
	if(! c) return Qnil;

	return wrap_cur(c, self, vtable);
}

void
Init_ptnk()
{
	RBM_ptnk = rb_define_module("Ptnk");
	#define WRAP_CONST(C) rb_define_const(RBM_ptnk, #C, INT2FIX(ptnk::C));
	WRAP_CONST(OWRITER);
	WRAP_CONST(OCREATE);
	WRAP_CONST(OTRUNCATE);
	WRAP_CONST(OAUTOSYNC);
	WRAP_CONST(OPARTITIONED);
	WRAP_CONST(ODEFAULT);
	WRAP_CONST(PUT_INSERT);
	WRAP_CONST(PUT_UPDATE);
	WRAP_CONST(PUT_LEAVE_EXISTING);
	WRAP_CONST(MATCH_EXACT);
	WRAP_CONST(MATCH_OR_PREV);
	WRAP_CONST(MATCH_OR_NEXT);
	WRAP_CONST(BEFORE);
	WRAP_CONST(AFTER);
	WRAP_CONST(FRONT);
	WRAP_CONST(BACK);
	#undef WRAP_CONST

	RBK_db = rb_define_class_under(RBM_ptnk, "DB", rb_cObject);
	
	rb_define_singleton_method(RBK_db, "new", (ruby_method_t)db_new, -1);
	rb_define_singleton_method(RBK_db, "open", (ruby_method_t)db_new, -1);
	rb_define_method(RBK_db, "close", (ruby_method_t)db_close, 0);
	rb_define_method(RBK_db, "get", (ruby_method_t)db_get, 1);
	rb_define_method(RBK_db, "put", (ruby_method_t)db_put, -1);

	RBK_dbtx = rb_define_class_under(RBK_db, "Tx", rb_cObject);
	rb_define_singleton_method(RBK_dbtx, "new", (ruby_method_t)dbtx_new, 1);
	rb_define_method(RBK_dbtx, "try_commit!", (ruby_method_t)dbtx_try_commit, -1);
	rb_define_method(RBK_dbtx, "abort!", (ruby_method_t)dbtx_abort, 0);

	rb_define_method(RBK_dbtx, "table_get_names", (ruby_method_t)dbtx_table_get_names, 0);

	RBK_table = rb_define_class_under(RBM_ptnk, "Table", rb_cObject);
	rb_define_singleton_method(RBK_table, "new", (ruby_method_t)table_new, 1);

	rb_define_method(RBK_dbtx, "table_create", (ruby_method_t)dbtx_table_create, 1);
	rb_define_method(RBK_dbtx, "table_drop", (ruby_method_t)dbtx_table_drop, 1);

	rb_define_method(RBK_dbtx, "get", (ruby_method_t)dbtx_get, -1);
	rb_define_method(RBK_dbtx, "put", (ruby_method_t)dbtx_put, -1);

	RBK_cur = rb_define_class_under(RBK_dbtx, "Cursor", rb_cObject);
	rb_define_method(RBK_cur, "get", (ruby_method_t)cur_get, 0);
	rb_define_method(RBK_cur, "next", (ruby_method_t)cur_next, 0);

	rb_define_method(RBK_dbtx, "cursor_front", (ruby_method_t)dbtx_cursor_front, 1);
	rb_define_method(RBK_dbtx, "cursor_query", (ruby_method_t)dbtx_cursor_query, -1);
}

} // extern "C"
