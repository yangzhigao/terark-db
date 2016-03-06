#include "nlt_store.hpp"
#include <terark/int_vector.hpp>
#include <terark/fast_zip_blob_store.hpp>
#include <typeinfo>
#include <float.h>

namespace terark { namespace db { namespace dfadb {

TERARK_DB_REGISTER_STORE("nlt", NestLoudsTrieStore);

NestLoudsTrieStore::NestLoudsTrieStore() {
}
NestLoudsTrieStore::~NestLoudsTrieStore() {
}

llong NestLoudsTrieStore::dataStorageSize() const {
	return m_store->mem_size();
}

llong NestLoudsTrieStore::dataInflateSize() const {
	return m_store->total_data_size();
}

llong NestLoudsTrieStore::numDataRows() const {
	return m_store->num_records();
}

void NestLoudsTrieStore::getValueAppend(llong id, valvec<byte>* val, DbContext* ctx) const {
	m_store->get_record_append(size_t(id), val);
}

StoreIterator* NestLoudsTrieStore::createStoreIterForward(DbContext*) const {
	return nullptr; // not needed
}

StoreIterator* NestLoudsTrieStore::createStoreIterBackward(DbContext*) const {
	return nullptr; // not needed
}

template<class Class>
static
Class* doBuild(const NestLoudsTrieConfig& conf,
			   const Schema& schema, SortableStrVec& strVec) {
	std::unique_ptr<Class> trie(new Class());
	trie->build_from(strVec, conf);
	return trie.release();
}

static
void initConfigFromSchema(NestLoudsTrieConfig& conf, const Schema& schema) {
	conf.initFromEnv();
	if (schema.m_sufarrMinFreq) {
		conf.saFragMinFreq = (byte_t)schema.m_sufarrMinFreq;
	}
	if (schema.m_minFragLen) {
		conf.minFragLen = schema.m_minFragLen;
	}
	if (schema.m_maxFragLen) {
		conf.maxFragLen = schema.m_maxFragLen;
	}
	if (schema.m_nltDelims.size()) {
		conf.setBestDelims(schema.m_nltDelims.c_str());
	}
	conf.nestLevel = schema.m_nltNestLevel;
}

static
BlobStore* nltBuild(const Schema& schema, SortableStrVec& strVec) {
	NestLoudsTrieConfig conf;
	initConfigFromSchema(conf, schema);
	switch (schema.m_rankSelectClass) {
	case -256:
		return doBuild<NestLoudsTrieBlobStore_IL>(conf, schema, strVec);
	case +256:
		return doBuild<NestLoudsTrieBlobStore_SE>(conf, schema, strVec);
	case +512:
		return doBuild<NestLoudsTrieBlobStore_SE_512>(conf, schema, strVec);
	default:
		fprintf(stderr, "WARN: invalid schema(%s).rs = %d, use default: se_512\n"
					  , schema.m_name.c_str(), schema.m_rankSelectClass);
		return doBuild<NestLoudsTrieBlobStore_SE_512>(conf, schema, strVec);
	}
}

void NestLoudsTrieStore::build(const Schema& schema, SortableStrVec& strVec) {
	if (schema.m_dictZipSampleRatio > 0) {
		std::unique_ptr<DictZipBlobStore> zds(new DictZipBlobStore());
		zds->build_none_local_match(strVec, schema.m_dictZipSampleRatio);
		m_store.reset(zds.release());
	}
	else if (schema.m_useFastZip) {
		std::unique_ptr<FastZipBlobStore> fzds(new FastZipBlobStore());
		NestLoudsTrieConfig  conf;
		initConfigFromSchema(conf, schema);
		fzds->build_from(strVec, conf);
		m_store.reset(fzds.release());
	}
	else {
		m_store.reset(nltBuild(schema, strVec));
	}
}

void
NestLoudsTrieStore::build_by_iter(const Schema& schema, PathRef fpath,
								  StoreIterator& iter,
								  const bm_uint_t* isDel,
								  const febitvec* isPurged) {
	std::unique_ptr<DictZipBlobStore> zds(new DictZipBlobStore());
	std::unique_ptr<DictZipBlobStore::ZipBuilder> builder(zds->createZipBuilder());
	double sampleRatio = schema.m_dictZipSampleRatio > FLT_EPSILON
					   ? schema.m_dictZipSampleRatio : 0.05;
	valvec<byte> rec;
	if (NULL == isPurged || isPurged->size() == 0) {
		llong recId;
		while (iter.increment(&recId, &rec)) {
			if (NULL == isDel || !terark_bit_test(isDel, recId)) {
				if (rand() < RAND_MAX * sampleRatio ) {
					builder->addSample(rec);
				}
			}
		}
		builder->prepare(recId + 1, fpath.string());
		iter.reset();
		while (iter.increment(&recId, &rec)) {
			if (NULL == isDel || !terark_bit_test(isDel, recId)) {
				builder->addRecord(rec);
			}
		}
	}
	else {
		llong  physicId = 0;
		size_t logicNum = isPurged->size();
		const bm_uint_t* isPurgedptr = isPurged->bldata();
		for (size_t logicId = 0; logicId < logicNum; ++logicId) {
			if (!terark_bit_test(isPurgedptr, logicId)) {
				bool hasData = iter.seekExact(physicId, &rec);
				TERARK_RT_assert(hasData, std::logic_error);
				if (NULL == isDel || !terark_bit_test(isDel, logicId)) {
					if (rand() < RAND_MAX * sampleRatio) {
						builder->addSample(rec);
					}
				}
				physicId++;
			}
		}
		builder->prepare(physicId, fpath.string());
		iter.reset();
		physicId = 0;
		for (size_t logicId = 0; logicId < logicNum; ++logicId) {
			if (!terark_bit_test(isPurgedptr, logicId)) {
				bool hasData = iter.increment(&physicId, &rec);
				TERARK_RT_assert(hasData, std::logic_error);
				if (NULL == isDel || !terark_bit_test(isDel, logicId)) {
					builder->addRecord(rec);
				}
				physicId++;
			}
		}
	}
	zds->completeBuild(*builder);
	m_store.reset(zds.release());
}

void NestLoudsTrieStore::load(PathRef path) {
	std::string fpath = fstring(path.string()).endsWith(".nlt")
					  ? path.string()
					  : path.string() + ".nlt";
	m_store.reset(BlobStore::load_from(fpath));
}

void NestLoudsTrieStore::save(PathRef path) const {
	std::string fpath = fstring(path.string()).endsWith(".nlt")
						? path.string()
						: path.string() + ".nlt";
	if (BaseDFA* dfa = dynamic_cast<BaseDFA*>(&*m_store)) {
		dfa->save_mmap(fpath.c_str());
	}
	else if (auto zds = dynamic_cast<FastZipBlobStore*>(&*m_store)) {
		zds->save_mmap(fpath);
	}
	else if (auto zds = dynamic_cast<DictZipBlobStore*>(&*m_store)) {
		zds->save_mmap(fpath);
	}
	else {
		THROW_STD(invalid_argument, "Unexpected");
	}
}

}}} // namespace terark::db::dfadb
