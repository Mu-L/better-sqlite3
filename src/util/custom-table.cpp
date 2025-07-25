class CustomTable {
public:

	explicit CustomTable(
		v8::Isolate* isolate,
		Database* db,
		const char* name,
		v8::Local<v8::Function> factory
	) :
		addon(db->GetAddon()),
		isolate(isolate),
		db(db),
		name(name),
		factory(isolate, factory) {}

	static void Destructor(void* self) {
		delete static_cast<CustomTable*>(self);
	}

	static sqlite3_module MODULE;
	static sqlite3_module EPONYMOUS_MODULE;

private:

	// This nested class is instantiated on each CREATE VIRTUAL TABLE statement.
	class VTab { friend class CustomTable;
		explicit VTab(
			CustomTable* parent,
			v8::Local<v8::Function> generator,
			std::vector<std::string> parameter_names,
			bool safe_ints
		) :
			parent(parent),
			parameter_count(parameter_names.size()),
			safe_ints(safe_ints),
			generator(parent->isolate, generator),
			parameter_names(parameter_names) {
			((void)base);
		}

		static inline CustomTable::VTab* Upcast(sqlite3_vtab* vtab) {
			return reinterpret_cast<VTab*>(vtab);
		}

		inline sqlite3_vtab* Downcast() {
			return reinterpret_cast<sqlite3_vtab*>(this);
		}

		sqlite3_vtab base;
		CustomTable * const parent;
		const int parameter_count;
		const bool safe_ints;
		const v8::Global<v8::Function> generator;
		const std::vector<std::string> parameter_names;
	};

	// This nested class is instantiated each time a virtual table is scanned.
	class Cursor { friend class CustomTable;
		static inline CustomTable::Cursor* Upcast(sqlite3_vtab_cursor* cursor) {
			return reinterpret_cast<Cursor*>(cursor);
		}

		inline sqlite3_vtab_cursor* Downcast() {
			return reinterpret_cast<sqlite3_vtab_cursor*>(this);
		}

		inline CustomTable::VTab* GetVTab() {
			return VTab::Upcast(base.pVtab);
		}

		sqlite3_vtab_cursor base;
		v8::Global<v8::Object> iterator;
		v8::Global<v8::Function> next;
		v8::Global<v8::Array> row;
		bool done;
		sqlite_int64 rowid;
	};

	// This nested class is used by Data::ResultValueFromJS to report errors.
	class TempDataConverter : DataConverter { friend class CustomTable;
		explicit TempDataConverter(CustomTable* parent) :
			parent(parent),
			status(SQLITE_OK) {}

		void PropagateJSError(sqlite3_context* invocation) {
			status = SQLITE_ERROR;
			parent->PropagateJSError();
		}

		std::string GetDataErrorPrefix() {
			return std::string("Virtual table module \"") + parent->name + "\" yielded";
		}

		CustomTable * const parent;
		int status;
	};

	// Although this function does nothing, we cannot use xConnect directly,
	// because that would cause SQLite to register an eponymous virtual table.
	static int xCreate(sqlite3* db_handle, void* _self, int argc, const char* const * argv, sqlite3_vtab** output, char** errOutput) {
		return xConnect(db_handle, _self, argc, argv, output, errOutput);
	}

	// This method uses the factory function to instantiate a new virtual table.
	static int xConnect(sqlite3* db_handle, void* _self, int argc, const char* const * argv, sqlite3_vtab** output, char** errOutput) {
		CustomTable* self = static_cast<CustomTable*>(_self);
		v8::Isolate* isolate = self->isolate;
		v8::HandleScope scope(isolate);
		UseContext;

		v8::Local<v8::Value>* args = ALLOC_ARRAY<v8::Local<v8::Value>>(argc);
		for (int i = 0; i < argc; ++i) {
			args[i] = StringFromUtf8(isolate, argv[i], -1);
		}

		// Run the factory function to receive a new virtual table definition.
		v8::MaybeLocal<v8::Value> maybeReturnValue = self->factory.Get(isolate)->Call(ctx, v8::Undefined(isolate), argc, args);
		delete[] args;

		if (maybeReturnValue.IsEmpty()) {
			self->PropagateJSError();
			return SQLITE_ERROR;
		}

		// Extract each part of the virtual table definition.
		v8::Local<v8::Array> returnValue = maybeReturnValue.ToLocalChecked().As<v8::Array>();
		v8::Local<v8::String> sqlString = returnValue->Get(ctx, 0).ToLocalChecked().As<v8::String>();
		v8::Local<v8::Function> generator = returnValue->Get(ctx, 1).ToLocalChecked().As<v8::Function>();
		v8::Local<v8::Array> parameterNames = returnValue->Get(ctx, 2).ToLocalChecked().As<v8::Array>();
		int safe_ints = returnValue->Get(ctx, 3).ToLocalChecked().As<v8::Int32>()->Value();
		bool direct_only = returnValue->Get(ctx, 4).ToLocalChecked().As<v8::Boolean>()->Value();

		v8::String::Utf8Value sql(isolate, sqlString);
		safe_ints = safe_ints < 2 ? safe_ints : static_cast<int>(self->db->GetState()->safe_ints);

		// Copy the parameter names into a std::vector.
		std::vector<std::string> parameter_names;
		for (int i = 0, len = parameterNames->Length(); i < len; ++i) {
			v8::Local<v8::String> parameterName = parameterNames->Get(ctx, i).ToLocalChecked().As<v8::String>();
			v8::String::Utf8Value parameter_name(isolate, parameterName);
			parameter_names.emplace_back(*parameter_name);
		}

		// Pass our SQL table definition to SQLite (this should never fail).
		if (sqlite3_declare_vtab(db_handle, *sql) != SQLITE_OK) {
			*errOutput = sqlite3_mprintf("failed to declare virtual table \"%s\"", argv[2]);
			return SQLITE_ERROR;
		}
		if (direct_only && sqlite3_vtab_config(db_handle, SQLITE_VTAB_DIRECTONLY) != SQLITE_OK) {
			*errOutput = sqlite3_mprintf("failed to configure virtual table \"%s\"", argv[2]);
			return SQLITE_ERROR;
		}

		// Return the successfully created virtual table.
		*output = (new VTab(self, generator, parameter_names, safe_ints))->Downcast();
		return SQLITE_OK;
	}

	static int xDisconnect(sqlite3_vtab* vtab) {
		delete VTab::Upcast(vtab);
		return SQLITE_OK;
	}

	static int xOpen(sqlite3_vtab* vtab, sqlite3_vtab_cursor** output) {
		*output = (new Cursor())->Downcast();
		return SQLITE_OK;
	}

	static int xClose(sqlite3_vtab_cursor* cursor) {
		delete Cursor::Upcast(cursor);
		return SQLITE_OK;
	}

	// This method uses a fresh cursor to start a new scan of a virtual table.
	// The args and idxNum are provided by xBestIndex (idxStr is unused).
	// idxNum is a bitmap that provides the proper indices of the received args.
	static int xFilter(sqlite3_vtab_cursor* _cursor, int idxNum, const char* idxStr, int argc, sqlite3_value** argv) {
		Cursor* cursor = Cursor::Upcast(_cursor);
		VTab* vtab = cursor->GetVTab();
		CustomTable* self = vtab->parent;
		Addon* addon = self->addon;
		v8::Isolate* isolate = self->isolate;
		v8::HandleScope scope(isolate);
		UseContext;

		// Convert the SQLite arguments into JavaScript arguments. Note that
		// the values in argv may be in the wrong order, so we fix that here.
		v8::Local<v8::Value> args_fast[4];
		v8::Local<v8::Value>* args = NULL;
		int parameter_count = vtab->parameter_count;
		if (parameter_count != 0) {
			args = parameter_count <= 4 ? args_fast : ALLOC_ARRAY<v8::Local<v8::Value>>(parameter_count);
			int argn = 0;
			bool safe_ints = vtab->safe_ints;
			for (int i = 0; i < parameter_count; ++i) {
				if (idxNum & 1 << i) {
					args[i] = Data::GetValueJS(isolate, argv[argn++], safe_ints);
					// If any arguments are NULL, the result set is necessarily
					// empty, so don't bother to run the generator function.
					if (args[i]->IsNull()) {
						if (args != args_fast) delete[] args;
						cursor->done = true;
						return SQLITE_OK;
					}
				} else {
					args[i] = v8::Undefined(isolate);
				}
			}
		}

		// Invoke the generator function to create a new iterator.
		v8::MaybeLocal<v8::Value> maybeIterator = vtab->generator.Get(isolate)->Call(ctx, v8::Undefined(isolate), parameter_count, args);
		if (args != args_fast) delete[] args;

		if (maybeIterator.IsEmpty()) {
			self->PropagateJSError();
			return SQLITE_ERROR;
		}

		// Store the iterator and its next() method; we'll be using it a lot.
		v8::Local<v8::Object> iterator = maybeIterator.ToLocalChecked().As<v8::Object>();
		v8::Local<v8::Function> next = iterator->Get(ctx, addon->cs.next.Get(isolate)).ToLocalChecked().As<v8::Function>();
		cursor->iterator.Reset(isolate, iterator);
		cursor->next.Reset(isolate, next);
		cursor->rowid = 0;

		// Advance the iterator/cursor to the first row.
		return xNext(cursor->Downcast());
	}

	// This method advances a virtual table's cursor to the next row.
	// SQLite will call this method repeatedly, driving the generator function.
	static int xNext(sqlite3_vtab_cursor* _cursor) {
		Cursor* cursor = Cursor::Upcast(_cursor);
		CustomTable* self = cursor->GetVTab()->parent;
		Addon* addon = self->addon;
		v8::Isolate* isolate = self->isolate;
		v8::HandleScope scope(isolate);
		UseContext;

		v8::Local<v8::Object> iterator = cursor->iterator.Get(isolate);
		v8::Local<v8::Function> next = cursor->next.Get(isolate);

		v8::MaybeLocal<v8::Value> maybeRecord = next->Call(ctx, iterator, 0, NULL);
		if (maybeRecord.IsEmpty()) {
			self->PropagateJSError();
			return SQLITE_ERROR;
		}

		v8::Local<v8::Object> record = maybeRecord.ToLocalChecked().As<v8::Object>();
		bool done = record->Get(ctx, addon->cs.done.Get(isolate)).ToLocalChecked().As<v8::Boolean>()->Value();
		if (!done) {
			cursor->row.Reset(isolate, record->Get(ctx, addon->cs.value.Get(isolate)).ToLocalChecked().As<v8::Array>());
		}
		cursor->done = done;
		cursor->rowid += 1;

		return SQLITE_OK;
	}

	// If this method returns 1, SQLite will stop scanning the virtual table.
	static int xEof(sqlite3_vtab_cursor* cursor) {
		return Cursor::Upcast(cursor)->done;
	}

	// This method extracts some column from the cursor's current row.
	static int xColumn(sqlite3_vtab_cursor* _cursor, sqlite3_context* invocation, int column) {
		Cursor* cursor = Cursor::Upcast(_cursor);
		CustomTable* self = cursor->GetVTab()->parent;
		TempDataConverter temp_data_converter(self);
		v8::Isolate* isolate = self->isolate;
		v8::HandleScope scope(isolate);

		v8::Local<v8::Array> row = cursor->row.Get(isolate);
		v8::MaybeLocal<v8::Value> maybeColumnValue = row->Get(OnlyContext, column);
		if (maybeColumnValue.IsEmpty()) {
			temp_data_converter.PropagateJSError(NULL);
		} else {
			Data::ResultValueFromJS(isolate, invocation, maybeColumnValue.ToLocalChecked(), &temp_data_converter);
		}
		return temp_data_converter.status;
	}

	// This method outputs the rowid of the cursor's current row.
	static int xRowid(sqlite3_vtab_cursor* cursor, sqlite_int64* output) {
		*output = Cursor::Upcast(cursor)->rowid;
		return SQLITE_OK;
	}

	// This method tells SQLite how to *plan* queries on our virtual table.
	// It gets invoked (typically multiple times) during db.prepare().
	static int xBestIndex(sqlite3_vtab* vtab, sqlite3_index_info* output) {
		int parameter_count = VTab::Upcast(vtab)->parameter_count;
		int argument_count = 0;
		std::vector<std::pair<int, int>> forwarded;

		for (int i = 0, len = output->nConstraint; i < len; ++i) {
			auto item = output->aConstraint[i];

			// The SQLITE_INDEX_CONSTRAINT_LIMIT and SQLITE_INDEX_CONSTRAINT_OFFSET
			// operators have no left-hand operand, and so for those operators the
			// corresponding item.iColumn is meaningless.
			// We don't care those constraints.
			if (item.op == SQLITE_INDEX_CONSTRAINT_LIMIT || item.op == SQLITE_INDEX_CONSTRAINT_OFFSET) {
				continue;
			}
			// We only care about constraints on parameters, not regular columns.
			if (item.iColumn >= 0 && item.iColumn < parameter_count) {
				if (item.op != SQLITE_INDEX_CONSTRAINT_EQ) {
					sqlite3_free(vtab->zErrMsg);
					vtab->zErrMsg = sqlite3_mprintf(
						"virtual table parameter \"%s\" can only be constrained by the '=' operator",
						VTab::Upcast(vtab)->parameter_names.at(item.iColumn).c_str());
					return SQLITE_ERROR;
				}
				if (!item.usable) {
					// Don't allow SQLite to make plans that ignore arguments.
					// Otherwise, a user could pass arguments, but then they
					// could appear undefined in the generator function.
					return SQLITE_CONSTRAINT;
				}
				forwarded.emplace_back(item.iColumn, i);
			}
		}

		// Tell SQLite to forward arguments to xFilter.
		std::sort(forwarded.begin(), forwarded.end());
		for (std::pair<int, int> pair : forwarded) {
			int bit = 1 << pair.first;
			if (!(output->idxNum & bit)) {
				output->idxNum |= bit;
				output->aConstraintUsage[pair.second].argvIndex = ++argument_count;
				output->aConstraintUsage[pair.second].omit = 1;
			}
		}

		// Use a very high estimated cost so SQLite is not tempted to invoke the
		// generator function within a loop, if it can be avoided.
		output->estimatedCost = output->estimatedRows = 1000000000 / (argument_count + 1);
		return SQLITE_OK;
	}

	void PropagateJSError() {
		assert(db->GetState()->was_js_error == false);
		db->GetState()->was_js_error = true;
	}

	Addon* const addon;
	v8::Isolate* const isolate;
	Database* const db;
	const std::string name;
	const v8::Global<v8::Function> factory;
};

sqlite3_module CustomTable::MODULE = {
	0,                            /* iVersion */
	xCreate,                      /* xCreate */
	xConnect,                     /* xConnect */
	xBestIndex,                   /* xBestIndex */
	xDisconnect,                  /* xDisconnect */
	xDisconnect,                  /* xDestroy */
	xOpen,                        /* xOpen */
	xClose,                       /* xClose */
	xFilter,                      /* xFilter */
	xNext,                        /* xNext */
	xEof,                         /* xEof */
	xColumn,                      /* xColumn */
	xRowid,                       /* xRowid */
	NULL,                         /* xUpdate */
	NULL,                         /* xBegin */
	NULL,                         /* xSync */
	NULL,                         /* xCommit */
	NULL,                         /* xRollback */
	NULL,                         /* xFindMethod */
	NULL,                         /* xRename */
	NULL,                         /* xSavepoint */
	NULL,                         /* xRelease */
	NULL,                         /* xRollbackTo */
	NULL,                         /* xShadowName */
	NULL                          /* xIntegrity */
};

sqlite3_module CustomTable::EPONYMOUS_MODULE = {
	0,                            /* iVersion */
	NULL,                         /* xCreate */
	xConnect,                     /* xConnect */
	xBestIndex,                   /* xBestIndex */
	xDisconnect,                  /* xDisconnect */
	xDisconnect,                  /* xDestroy */
	xOpen,                        /* xOpen */
	xClose,                       /* xClose */
	xFilter,                      /* xFilter */
	xNext,                        /* xNext */
	xEof,                         /* xEof */
	xColumn,                      /* xColumn */
	xRowid,                       /* xRowid */
	NULL,                         /* xUpdate */
	NULL,                         /* xBegin */
	NULL,                         /* xSync */
	NULL,                         /* xCommit */
	NULL,                         /* xRollback */
	NULL,                         /* xFindMethod */
	NULL,                         /* xRename */
	NULL,                         /* xSavepoint */
	NULL,                         /* xRelease */
	NULL,                         /* xRollbackTo */
	NULL,                         /* xShadowName */
	NULL                          /* xIntegrity */
};
