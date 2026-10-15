#ifdef _WIN32
#include <windows.h>
#endif

#include "obs.h"
#include "obs-module.h"

#include <map>
#include <set>
#include <list>
#include <mutex>
#include <array>
#include <string>
#include <vector>
#include <memory>
#include <numeric>
#include <algorithm>
#include <cstdint>
#include <functional>
#include <cmath>
#include <cstring>

extern "C" {
#include "libavfilter/avfilter.h"
#include "libavfilter/buffersrc.h"
#include "libavfilter/buffersink.h"
#include "libavutil/avutil.h"
#include "libavutil/opt.h"
#include "libavutil/channel_layout.h"
}

#define TAG "[ffmpeg-afilter] "

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-ffmpeg-afilters", "en-US");

MODULE_EXPORT const char *obs_module_description(void)
{
	return "FFMPEG Audio Filters";
}

using namespace std;

set<string> g_filter_black_list_{
	"abench",     "afade",         "afifo",         "aloop",
	"ametadata",  "aperms",        "areverse",      "anull",
	"replaygain", "silencedetect", "silenceremove", "volumedetect"};

static bool is_default_value(obs_data_t *data, const char *name)
{
	obs_data_item_t *item = obs_data_item_byname(data, name);
	if (!item)
		return true;

	bool is_default = false;
	if (!obs_data_item_has_user_value(item)) {
		is_default = true;
	} else if (obs_data_item_has_default_value(item)) {
		switch (obs_data_item_gettype(item)) {
		case OBS_DATA_STRING: {
			const char *val = obs_data_item_get_string(item);
			const char *def =
				obs_data_item_get_default_string(item);
			is_default = val && def && strcmp(val, def) == 0;
			break;
		}
		case OBS_DATA_NUMBER:
			if (obs_data_item_numtype(item) == OBS_DATA_NUM_INT)
				is_default = obs_data_item_get_int(item) ==
					     obs_data_item_get_default_int(item);
			else
				is_default = obs_data_item_get_double(item) ==
					     obs_data_item_get_default_double(item);
			break;
		case OBS_DATA_BOOLEAN:
			is_default = obs_data_item_get_bool(item) ==
				     obs_data_item_get_default_bool(item);
			break;
		default:
			is_default = false;
			break;
		}
	}

	obs_data_item_release(&item);
	return is_default;
}

static void copy_data_item(obs_data_t *dst, obs_data_t *src, const char *name)
{
	obs_data_item_t *item = obs_data_item_byname(src, name);
	if (!item)
		return;

	switch (obs_data_item_gettype(item)) {
	case OBS_DATA_STRING:
		obs_data_set_string(dst, name, obs_data_item_get_string(item));
		break;
	case OBS_DATA_NUMBER:
		if (obs_data_item_numtype(item) == OBS_DATA_NUM_INT)
			obs_data_set_int(dst, name,
					 obs_data_item_get_int(item));
		else
			obs_data_set_double(dst, name,
					    obs_data_item_get_double(item));
		break;
	case OBS_DATA_BOOLEAN:
		obs_data_set_bool(dst, name, obs_data_item_get_bool(item));
		break;
	case OBS_DATA_OBJECT: {
		obs_data_t *obj = obs_data_item_get_obj(item);
		obs_data_set_obj(dst, name, obj);
		obs_data_release(obj);
		break;
	}
	case OBS_DATA_ARRAY: {
		obs_data_array_t *arr = obs_data_item_get_array(item);
		obs_data_set_array(dst, name, arr);
		obs_data_array_release(arr);
		break;
	}
	default:
		break;
	}

	obs_data_item_release(&item);
}

// Pick a step small enough for the spinbox to expose the decimals the option
// range needs (e.g. 0.00001 for anlmdn's strength), instead of a fixed 0.1.
static double choose_float_step(double min, double max, double default_val)
{
	auto decimals_of = [](double v) {
		v = fabs(v);
		if (v == 0.0)
			return 0;
		for (int d = 0; d <= 6; ++d) {
			if (fabs(v - round(v)) < 1e-9 * (std::max)(1.0, v))
				return d;
			v *= 10.0;
		}
		return 6;
	};

	int decimals = (std::max)({decimals_of(min), decimals_of(max),
				   decimals_of(default_val)});
	decimals = (std::min)(decimals, 6);
	return pow(10.0, -decimals);
}

#if defined(_WIN32)

static bool set_clipboard_text(const std::string &text)
{
	if (!OpenClipboard(nullptr))
		return false;

	EmptyClipboard();

	bool ok = false;
	int wlen = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr,
				       0);
	if (wlen > 0) {
		HANDLE mem = GlobalAlloc(
			GMEM_MOVEABLE,
			static_cast<SIZE_T>(wlen) * sizeof(wchar_t));
		if (mem) {
			wchar_t *dst =
				static_cast<wchar_t *>(GlobalLock(mem));
			if (dst) {
				MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
						    -1, dst, wlen);
				GlobalUnlock(mem);
				ok = SetClipboardData(CF_UNICODETEXT, mem) !=
				     nullptr;
			}
			if (!ok)
				GlobalFree(mem);
		}
	}

	CloseClipboard();
	return ok;
}

static std::string get_clipboard_text()
{
	std::string result;

	if (!IsClipboardFormatAvailable(CF_UNICODETEXT) ||
	    !OpenClipboard(nullptr))
		return result;

	HANDLE mem = GetClipboardData(CF_UNICODETEXT);
	if (mem) {
		const wchar_t *src =
			static_cast<const wchar_t *>(GlobalLock(mem));
		if (src) {
			int len = WideCharToMultiByte(CP_UTF8, 0, src, -1,
						      nullptr, 0, nullptr,
						      nullptr);
			if (len > 1) {
				std::vector<char> buf(len);
				WideCharToMultiByte(CP_UTF8, 0, src, -1,
						    buf.data(), len, nullptr,
						    nullptr);
				result.assign(buf.data(), len - 1);
			}
			GlobalUnlock(mem);
		}
	}

	CloseClipboard();
	return result;
}

#else

static bool set_clipboard_text(const std::string &)
{
	blog(LOG_WARNING, TAG "clipboard is not supported on this platform");
	return false;
}

static std::string get_clipboard_text()
{
	blog(LOG_WARNING, TAG "clipboard is not supported on this platform");
	return {};
}

#endif

template<class T> class IAudioFilter {
protected:
	obs_properties_t *properties() { return obs_properties_create(); }

	void update(obs_data_t *s) {}

public:
	static void Destroy(void *data)
	{
		static_cast<T *>(data)->~T();
		bfree(data);
	}

	static void Update(void *data, obs_data_t *s)
	{
		static_cast<T *>(data)->update(s);
	}

	static void *Create(obs_data_t *settings, obs_source_t *filter)
	{
		void *memBlock = bzalloc(sizeof(T));
		T *thiz = new (memBlock) T(settings, filter);
		return thiz;
	}

	static obs_audio_data *FilterAudio(void *data, obs_audio_data *audio)
	{
		return static_cast<T *>(data)->filterAudio(audio);
	}

	static void Defaults(obs_data_t *settings) {}

	static obs_properties_t *Properties(void *data)
	{
		return static_cast<T *>(data)->properties();
	}
};

template<class T> void RegisterFilter()
{
	obs_source_info info{};
	info.id = T::Id();
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_AUDIO;
	info.get_name = &T::Name;
	info.create = &T::Create;
	info.destroy = &T::Destroy;
	info.update = &T::Update;
	info.filter_audio = &T::FilterAudio;
	info.get_defaults = &T::Defaults;
	info.get_properties = &T::Properties;
	obs_register_source(&info);
}

// ========== filter list
class FFAFilterCollection {
public:
	using CollectionType = map<string, const AVFilter *>;

	static FFAFilterCollection &Get();

	bool Init();
	CollectionType::const_iterator begin() const;
	CollectionType::const_iterator end() const;
	CollectionType::const_iterator find(const string &key) const;

private:
	CollectionType filter_list_;
};

// ========== filter options
class FFAFilterOpts {
	// for global
public:
	using UPtr = unique_ptr<FFAFilterOpts>;
	static const FFAFilterOpts *Get(const string &name);

private:
	static mutex cache_mutex_;
	static map<string, FFAFilterOpts::UPtr> cache_;

	// for instance
public:
	struct UnitDef {
		const char *name;
		int64_t val;
		const char *help;
	};

	using Units = vector<UnitDef>;
	using UnitsPtr = shared_ptr<Units>;

	struct Opt {
		const char *name;
		const char *help;
		double max;
		double min;
		int type;
		decltype(AVOption::default_val) default_val;
		UnitsPtr units;
	};

	void AddToProperties(obs_properties *props, obs_data *settings, const obs_audio_info &oai) const;
	AVDictionary *LoadOBSSettings(AVFilterContext *filterCtx, obs_data *settings) const;

private:
	FFAFilterOpts() {}
	bool Load(const AVFilter *filter);

	string filter_name_;
	list<Opt> opts_;
	map<string, UnitsPtr> units_;
};

// ========== filter
class FilterGraph {
	AVFilterGraph *graph_ = nullptr;
	AVFilterContext *inputFilter_ = nullptr;
	AVFilterContext *mainFilter_ = nullptr;
	AVFilterContext *outputFilter_ = nullptr;

	obs_audio_info oai_ = obs_audio_info{};

public:
	FilterGraph() {}
	~FilterGraph();

	bool initFilter(obs_data *settings, const obs_audio_info *oai);

	obs_audio_data *
	filterAudio(obs_audio_data *audio, array<vector<float>, MAX_AV_PLANES> &frame_datas, bool end);
};

class auto_adapt_string {
	std::string buf_;

public:
	auto_adapt_string() {}

	auto_adapt_string(const char *str)
	{
		if (str)
			buf_ = str;
	}

	auto_adapt_string(const string &str) { buf_ = str; }

	template<class T> auto_adapt_string(const T &val)
	{
		buf_ = to_string(val);
	}

	operator const string &() const { return buf_; }
	operator const char *() const { return buf_.c_str(); };
};

AVChannelLayout speaker_layout_to_av_ch_layout(speaker_layout x)
{
	switch (x) {
	case speaker_layout::SPEAKERS_MONO:
		return AV_CHANNEL_LAYOUT_MONO;
	case speaker_layout::SPEAKERS_UNKNOWN:
	case speaker_layout::SPEAKERS_STEREO:
		return AV_CHANNEL_LAYOUT_STEREO;
	case speaker_layout::SPEAKERS_2POINT1:
		return AV_CHANNEL_LAYOUT_2POINT1;
	case speaker_layout::SPEAKERS_4POINT0:
		return AV_CHANNEL_LAYOUT_4POINT0;
	case speaker_layout::SPEAKERS_4POINT1:
		return AV_CHANNEL_LAYOUT_4POINT1;
	case speaker_layout::SPEAKERS_5POINT1:
		return AV_CHANNEL_LAYOUT_5POINT1;
	case speaker_layout::SPEAKERS_7POINT1:
		return AV_CHANNEL_LAYOUT_7POINT1;
	default:
		return speaker_layout_to_av_ch_layout(
			speaker_layout::SPEAKERS_UNKNOWN);
	}
}

std::string to_string(const AVChannelLayout& chLayout) {
	char str[64] = {0};
	av_channel_layout_describe(&chLayout, str, sizeof(str));
	return str;
}

speaker_layout av_ch_layout_to_speaker_layout(int x)
{
	switch (x) {
	case AV_CH_LAYOUT_MONO:
		return speaker_layout::SPEAKERS_MONO;
	case AV_CH_LAYOUT_STEREO:
		return speaker_layout::SPEAKERS_STEREO;
	case AV_CH_LAYOUT_2POINT1:
		return speaker_layout::SPEAKERS_2POINT1;
	case AV_CH_LAYOUT_4POINT0:
		return speaker_layout::SPEAKERS_4POINT0;
	case AV_CH_LAYOUT_4POINT1:
		return speaker_layout::SPEAKERS_4POINT1;
	case AV_CH_LAYOUT_5POINT1:
		return speaker_layout::SPEAKERS_5POINT1;
	case AV_CH_LAYOUT_7POINT1:
		return speaker_layout::SPEAKERS_7POINT1;
	default:
		return speaker_layout::SPEAKERS_UNKNOWN;
	}
}

class FFAFilter : public IAudioFilter<FFAFilter> {
	static bool obs_properties_add_avfilter(
		obs_properties *props,
		obs_data *settings,
		const obs_audio_info &oai,
		const AVFilter *filter);

	static bool obs_properties_hide_other_avclass(
		obs_properties *props,
		const AVFilter *filter);

	mutex graph_mutex_;
	unique_ptr<FilterGraph> filter_graph_;
	unique_ptr<FilterGraph> next_filter_graph_;
	string current_filter_name_;
	array<vector<float>, MAX_AV_PLANES> last_audio_data_bufs_;
	obs_audio_data last_audio_data_;
	obs_source_t *source_ = nullptr;

public:
	FFAFilter(obs_data_t *settings, obs_source_t *filter);
	~FFAFilter() {}

	static const char *Name(void *type_data)
	{
		return "FFMPEG AudioFilter";
	}

	static const char *Id() { return "ffmpeg-audio-filter"; }

	void update(obs_data_t *s);
	obs_audio_data *filterAudio(obs_audio_data *audio);
	obs_properties_t *properties();

	static bool ExportToClipboardClicked(obs_properties_t *props,
					     obs_property_t *property,
					     void *data);
	static bool ImportFromClipboardClicked(obs_properties_t *props,
					       obs_property_t *property,
					       void *data);
};

FFAFilterCollection &FFAFilterCollection::Get()
{
	static FFAFilterCollection instance;
	return instance;
}

bool FFAFilterCollection::Init()
{
	const AVFilter *flt = NULL;
	void *opaque = nullptr;
	bool has_filter_found = false;

#if LIBAVFILTER_VERSION_MAJOR >= 7
	while (flt = av_filter_iterate(&opaque))
#else
	while ((flt = avfilter_next(flt)) != NULL)
#endif
	{
		if (g_filter_black_list_.find(flt->name) !=
		    g_filter_black_list_.end())
			continue;
		if (avfilter_filter_pad_count(flt, true) != 1)
			continue;
		if (avfilter_filter_pad_count(flt, false) != 1)
			continue;
		auto inpad_type = avfilter_pad_get_type(flt->inputs, 0);
		if (!(inpad_type & AVMediaType::AVMEDIA_TYPE_AUDIO))
			continue;
		auto outpad_type = avfilter_pad_get_type(flt->outputs, 0);
		if (!(outpad_type & AVMEDIA_TYPE_AUDIO))
			continue;

		filter_list_.insert(make_pair(flt->name, flt));
		has_filter_found = true;
	}

	return true;
}

FFAFilterCollection::CollectionType::const_iterator
FFAFilterCollection::begin() const
{
	return filter_list_.begin();
}

FFAFilterCollection::CollectionType::const_iterator
FFAFilterCollection::end() const
{
	return filter_list_.end();
}

FFAFilterCollection::CollectionType::const_iterator
FFAFilterCollection::find(const string &key) const
{
	return filter_list_.find(key);
}

const FFAFilterOpts *FFAFilterOpts::Get(const string &name)
{
	lock_guard<mutex> lg(cache_mutex_);
	{ // cached
		auto it = cache_.find(name);
		if (it != cache_.end())
			return it->second.get();
	}
	{ // cache miss
		auto &newItem = cache_[name];
		auto filterCollection = FFAFilterCollection::Get();
		auto it = filterCollection.find(name);
		if (it != filterCollection.end()) {
			auto tmpItem = FFAFilterOpts::UPtr(new FFAFilterOpts());
			if (tmpItem->Load(it->second)) {
				newItem.swap(tmpItem);
				return newItem.get();
			}
		}

		return nullptr;
	}
}

bool FFAFilterOpts::Load(const AVFilter *filter)
{
	if (!filter)
		return false;

	const AVOption* opts = nullptr;
	if (filter->priv_class && filter->priv_class->option)
		opts = filter->priv_class->option;

	if (!opts)
		return true;

	map<int, Opt *> offsets;
	filter_name_ = filter->name;

	for (int i = 0; opts[i].name; ++i) {
		auto &opt = opts[i];

		// check if alias
		if (opt.offset > 0) {
			auto it = offsets.find(opt.offset);
			if (it != offsets.end()) {
				int oldlen = strlen(it->second->name);
				int newlen = strlen(opt.name);
				if (newlen > oldlen)
					it->second->name = opt.name;
				continue;
			}
		}

		auto get_units = [this](const char *unitname) {
			auto it = units_.find(unitname);
			if (it == units_.end()) {
				auto &new_item = units_[unitname];
				new_item.reset(new vector<UnitDef>());
				return new_item;
			} else
				return it->second;
		};

		// for const
		if (opt.type == AV_OPT_TYPE_CONST) {
			get_units(opt.unit)->emplace_back(UnitDef{
				opt.name, opt.default_val.i64, opt.help});
		}

		// for options
		else {
			opts_.push_back({});
			auto &new_opt = opts_.back();

			new_opt.name = opt.name;
			new_opt.help = opt.help;
			new_opt.type = opt.type;
			new_opt.max = opt.max;
			new_opt.min = opt.min;
			new_opt.default_val = opt.default_val;

			if (opt.unit)
				new_opt.units = get_units(opt.unit);

			offsets[opt.offset] = &new_opt;
		}
	}

	return true;
}

static bool obs_property_set_visible(obs_properties *props,
				     const std::string &name)
{
	auto prop = obs_properties_get(props, name.c_str());
	if (prop) {
		obs_property_set_visible(prop, true);
		return true;
	}
	return false;
}

void FFAFilterOpts::AddToProperties(obs_properties *props, obs_data *settings,
				    const obs_audio_info &oai) const
{
	auto obs_properties_add_int_unit = [&](obs_properties *props, const Opt &opt, const string &optname) {
		auto unit = opt.units;
		auto prop = obs_properties_add_list(props, optname.c_str(), opt.name,
			obs_combo_type::OBS_COMBO_TYPE_LIST, obs_combo_format::OBS_COMBO_FORMAT_INT);
		
		for (auto x : *unit) {
			string itemstr;
			itemstr += x.name;
			if (x.help) {
				itemstr += ": ";
				itemstr += x.help;
			}
			obs_property_list_add_int(prop, itemstr.c_str(), x.val);
		}
	};

	auto obs_properties_add_flags = [&](const Opt &opt,
					    const string &optname) {
		auto units = opt.units;
		for (auto x : *units) {
			string flagname;
			flagname += optname;
			flagname += "+";
			flagname += x.name;

			string flaghelp;
			flaghelp += opt.name;
			flaghelp += ": ";
			if (x.help && *x.help)
				flaghelp += x.help;
			else
				flaghelp += x.name;

			if (obs_property_set_visible(props, flagname))
				continue;

			auto prop = obs_properties_add_bool(
				props, flagname.c_str(),
				flaghelp.c_str()); // enough space in ui to show desc
			bool default_val = (opt.default_val.i64 & x.val) != 0;
			obs_data_set_default_bool(settings, flagname.c_str(),
						  default_val);

			if (x.help)
				obs_property_set_long_description(prop, x.help);
		}
	};

	auto obs_property_list_add_chlayout = [&](const Opt &opt,
						  const string &optname) {
		auto prop = obs_properties_add_list(
			props, optname.c_str(), opt.name,
			obs_combo_type::OBS_COMBO_TYPE_LIST,
			obs_combo_format::OBS_COMBO_FORMAT_STRING);
		obs_property_list_add_string(prop, "Mono", to_string(AV_CHANNEL_LAYOUT_MONO).c_str());
		obs_property_list_add_string(prop, "Stereo", to_string(AV_CH_LAYOUT_STEREO).c_str());
		obs_property_list_add_string(prop, "2.1", to_string(AV_CH_LAYOUT_2POINT1).c_str());
		obs_property_list_add_string(prop, "4.0", to_string(AV_CH_LAYOUT_4POINT0).c_str());
		obs_property_list_add_string(prop, "4.1", to_string(AV_CH_LAYOUT_4POINT1).c_str());
		obs_property_list_add_string(prop, "5.1", to_string(AV_CH_LAYOUT_5POINT1).c_str());
		obs_property_list_add_string(prop, "7.1", to_string(AV_CH_LAYOUT_7POINT1).c_str());
	};

	auto obs_properties_add_regular_type = [&](const Opt &opt,
						   auto_adapt_string optname) {
		if (obs_property_set_visible(props, optname))
			return true;

		obs_property *prop = nullptr;

		if (opt.type == AV_OPT_TYPE_FLOAT ||
		    opt.type == AV_OPT_TYPE_DOUBLE ||
		    opt.type == AV_OPT_TYPE_DURATION) {
			double defval =
				opt.type == AV_OPT_TYPE_DURATION
					? static_cast<double>(opt.default_val.i64)
					: opt.default_val.dbl;
			double step =
				choose_float_step(opt.min, opt.max, defval);
			prop = obs_properties_add_float(props, optname,
							opt.name, opt.min,
							opt.max, step);
			obs_data_set_default_double(settings, optname, defval);
		} else if (opt.type == AV_OPT_TYPE_INT ||
			   opt.type == AV_OPT_TYPE_INT64) {
			if (opt.units) {
				obs_properties_add_int_unit(props, opt,
							    optname);
			} else {
				prop = obs_properties_add_int(
					props, optname, opt.name,
					static_cast<int>(opt.min),
					static_cast<int>(opt.max), 1);
			}
			obs_data_set_default_int(settings, optname,
						 opt.default_val.i64);
		} else if (opt.type == AV_OPT_TYPE_STRING) {
			prop = obs_properties_add_text(
				props, optname, opt.name,
				obs_text_type::OBS_TEXT_DEFAULT);
			obs_data_set_default_string(settings, optname,
						    opt.default_val.str);
		} else if (opt.type == AV_OPT_TYPE_BOOL) {
			prop = obs_properties_add_bool(props, optname,
						       opt.name);
			obs_data_set_default_bool(settings, optname,
						  opt.default_val.i64);
		} else if (opt.type == AV_OPT_TYPE_CHLAYOUT) {
			obs_property_list_add_chlayout(opt, optname);
			obs_data_set_default_string(settings, optname, to_string(speaker_layout_to_av_ch_layout(oai.speakers)).c_str());
		} else {
			blog(LOG_WARNING, TAG "unsupport type %d for %s",
			     opt.type, optname);
		}

		if (prop && opt.help)
			obs_property_set_long_description(prop, opt.help);
		return true;
	};

	for (auto &opt : opts_) {
		// construct option name
		auto_adapt_string optname = filter_name_ + "/" + opt.name;

		obs_property *prop = nullptr;

		// add property in correct type
		if (opt.type == AV_OPT_TYPE_FLAGS && opt.units)
			obs_properties_add_flags(opt, optname);
		else
			obs_properties_add_regular_type(opt, optname);
	}
}

AVDictionary *FFAFilterOpts::LoadOBSSettings(AVFilterContext *filterCtx,
					     obs_data *settings) const
{
	AVDictionary *optdict = nullptr;

	auto obs_data_get_str = [&](obs_data_item *item) -> auto_adapt_string {
		assert(item != nullptr);
		switch (obs_data_item_gettype(item)) {
		case OBS_DATA_NUMBER:
			switch (obs_data_item_numtype(item)) {
			case OBS_DATA_NUM_INT:
				return obs_data_item_get_int(item);
			case OBS_DATA_NUM_DOUBLE:
				return obs_data_item_get_double(item);
			default:
				assert(false);
				return {};
			}
		case OBS_DATA_STRING:
			return obs_data_item_get_string(item);
		case OBS_DATA_BOOLEAN:
			return obs_data_item_get_bool(item) ? 1 : 0;
		default:
			assert(false);
			return {};
		}
	};

	auto obs_data_to_regular_avopt = [&](const char *obsname,
					     const char *optname) {
		auto item = obs_data_item_byname(settings, obsname);
		if (item) {
			auto strval = obs_data_get_str(item);
			int err = av_dict_set(&optdict, optname, strval, 0);
			return true;
		} else
			return false;
	};

	auto obs_data_to_flag_avopt = [&](const char *optname,
					  const Units &units) {
		int64_t flags = 0;
		for (auto &x : units) {
			auto_adapt_string flagname =
				std::string(optname) + "+" + x.name;
			if (obs_data_has_user_value(settings, flagname))
				if (obs_data_get_bool(settings, flagname))
					flags |= x.val;
		}
		av_opt_set(&optdict, optname, auto_adapt_string(flags), 0);
		return true;
	};

	for (auto &opt : opts_) {
		// construct option name
		string optname = filter_name_ + "/" + opt.name;

		// add property in correct type
		if (opt.type == AV_OPT_TYPE_FLAGS && opt.units) {
			obs_data_to_flag_avopt(optname.c_str(), *opt.units);
		} else if (obs_data_has_user_value(settings, optname.c_str())) {
			obs_data_to_regular_avopt(optname.c_str(), opt.name);
		}
	}

	return optdict;
}

mutex FFAFilterOpts::cache_mutex_;
map<string, FFAFilterOpts::UPtr> FFAFilterOpts::cache_;

FilterGraph::~FilterGraph()
{
	avfilter_free(inputFilter_);
	avfilter_free(mainFilter_);
	avfilter_free(outputFilter_);
	avfilter_graph_free(&graph_);
}

bool FilterGraph::initFilter(obs_data *settings, const obs_audio_info *oai)
{
	auto filter_name = obs_data_get_string(settings, "filter_name");
	if (!filter_name || !*filter_name)
		return false;

	oai_ = *oai;

	const AVFilter *filter = avfilter_get_by_name(filter_name);
	if (!filter)
		return false;

	auto graph = avfilter_graph_alloc();
	AVFilterContext *input_buffer_filter = 0, *output_buffer_filter = 0,
			*main_filter = 0;
	AVDictionary *main_filter_optdict = 0;

	auto on_failed = [&]() -> bool {
		avfilter_free(input_buffer_filter);
		avfilter_free(output_buffer_filter);
		avfilter_free(main_filter);
		av_dict_free(&main_filter_optdict);
		avfilter_graph_free(&graph);
		return false;
	};

	// ====== init input buffer
	input_buffer_filter = avfilter_graph_alloc_filter(
		graph, avfilter_get_by_name("abuffer"), "src");
	av_opt_set(input_buffer_filter, "channel_layout", 
		to_string(speaker_layout_to_av_ch_layout(oai->speakers)).c_str(),
		AV_OPT_SEARCH_CHILDREN);
	av_opt_set(input_buffer_filter, "sample_fmt",
		av_get_sample_fmt_name(AV_SAMPLE_FMT_FLTP),
		AV_OPT_SEARCH_CHILDREN);
	av_opt_set_q(input_buffer_filter, "time_base",
		AVRational{1, 1000000000}, AV_OPT_SEARCH_CHILDREN);
	av_opt_set_int(input_buffer_filter, "sample_rate", oai->samples_per_sec,
		AV_OPT_SEARCH_CHILDREN);
	if (avfilter_init_str(input_buffer_filter, nullptr) < 0) {
		blog(LOG_WARNING, TAG "fail to apply options for inputbuffer");
		return on_failed();
	}

	// ====== init output buffer
	output_buffer_filter = avfilter_graph_alloc_filter(
		graph, avfilter_get_by_name("abuffersink"), "sink");
	av_opt_set(output_buffer_filter, "channel_layouts",
		to_string(speaker_layout_to_av_ch_layout(oai->speakers)).c_str(),
		AV_OPT_SEARCH_CHILDREN);
	av_opt_set(output_buffer_filter, "sample_formats",
		av_get_sample_fmt_name(AV_SAMPLE_FMT_FLTP),
		AV_OPT_SEARCH_CHILDREN);
	av_opt_set(output_buffer_filter, "samplerates",
		to_string(oai->samples_per_sec).c_str(),
		AV_OPT_SEARCH_CHILDREN);
	if (avfilter_init_str(output_buffer_filter, nullptr) < 0) {
		blog(LOG_WARNING, TAG "fail to apply options for outputbuffer");
		return on_failed();
	}

	// ====== init main filter
	main_filter = avfilter_graph_alloc_filter(graph, filter, "main_filter");
	auto filterOpts = FFAFilterOpts::Get(filter->name);
	if (!filterOpts) {
		blog(LOG_WARNING, TAG "fail to parse %s options", filter_name);
		return on_failed();
	}
	main_filter_optdict =
		filterOpts->LoadOBSSettings(main_filter, settings);
	if (avfilter_init_dict(main_filter, &main_filter_optdict) < 0) {
		blog(LOG_WARNING, TAG "fail to apply options for %s",
		     filter_name);
		return on_failed();
	}

	// ====== link filters
	if (avfilter_link(input_buffer_filter, 0, main_filter, 0) < 0) {
		blog(LOG_WARNING, TAG "fail to connect inputbuffer to %s",
		     filter_name);
		return on_failed();
	}
	if (avfilter_link(main_filter, 0, output_buffer_filter, 0) < 0) {
		blog(LOG_WARNING, TAG "fail to connect %s to outputbuffer",
		     filter_name);
		return on_failed();
	}

	// ====== config graph
	if (avfilter_graph_config(graph, nullptr) >= 0) {
		graph_ = graph;
		inputFilter_ = input_buffer_filter;
		mainFilter_ = main_filter;
		outputFilter_ = output_buffer_filter;
		blog(LOG_INFO, TAG "use %s", filter_name);
	} else {
		blog(LOG_WARNING, TAG "create %s failed", filter_name);
		return on_failed();
	}

	av_dict_free(&main_filter_optdict);

	return true;
}

obs_audio_data *
FilterGraph::filterAudio(obs_audio_data *audio,
			 array<vector<float>, MAX_AV_PLANES> &frame_datas,
			 bool end)
{
	AVFrame *fin = nullptr, *fout = nullptr;
	auto result = [&](obs_audio_data *audio) {
		av_frame_free(&fin);
		av_frame_free(&fout);
		return audio;
	};

	if (!graph_)
		return result(audio);

	// ====== feed data into graph
	fin = av_frame_alloc();
	if (!fin)
		return result(audio);
	fin->sample_rate = oai_.samples_per_sec;
	fin->format = AV_SAMPLE_FMT_FLTP;
	fin->ch_layout = speaker_layout_to_av_ch_layout(oai_.speakers);
	fin->nb_samples = audio->frames;
	fin->pts = audio->timestamp;
	fin->linesize[0] = audio->frames * sizeof(float);
	for (int i = 0; i < fin->ch_layout.nb_channels; ++i)
		fin->data[i] = audio->data[i];

	int inerr = av_buffersrc_add_frame_flags(inputFilter_, fin, 0);
	if (inerr < 0)
		return result(audio);
	if (end)
		av_buffersrc_add_frame_flags(inputFilter_, nullptr, 0);

	// ====== retrieve data from graph
	fout = av_frame_alloc();
	if (!fout)
		return result(audio);
	int outerr = av_buffersink_get_frame(outputFilter_, fout);
	if (outerr == AVERROR(EAGAIN))
		return result(nullptr);
	else if (end && outerr == AVERROR_EOF)
		return result(nullptr);
	else if (outerr < 0)
		return result(audio);

	const int channels = fout->ch_layout.nb_channels;
	const int expected_channels =
		speaker_layout_to_av_ch_layout(oai_.speakers).nb_channels;
	if (fout->format != AV_SAMPLE_FMT_FLTP || fout->nb_samples <= 0 ||
	    channels <= 0 || channels != expected_channels ||
	    channels > MAX_AV_PLANES ||
	    !fout->extended_data) {
		blog(LOG_WARNING,
		     TAG "invalid output frame: format=%d, channels=%d, samples=%d",
		     fout->format, channels, fout->nb_samples);
		return result(audio);
	}

	const size_t plane_size =
		static_cast<size_t>(fout->nb_samples) * sizeof(float);
	for (int i = 0; i < channels; ++i) {
		const uint8_t *plane_data = fout->extended_data[i];
		AVBufferRef *plane_buffer = av_frame_get_plane_buffer(fout, i);
		if (!plane_data || !plane_buffer || !plane_buffer->data) {
			blog(LOG_WARNING, TAG "missing output buffer for plane %d", i);
			return result(audio);
		}

		const uintptr_t buffer_start =
			reinterpret_cast<uintptr_t>(plane_buffer->data);
		const uintptr_t data_start = reinterpret_cast<uintptr_t>(plane_data);
		if (data_start < buffer_start ||
		    data_start - buffer_start > plane_buffer->size ||
		    plane_size > plane_buffer->size - (data_start - buffer_start)) {
			blog(LOG_WARNING,
			     TAG "output plane %d is smaller than %zu bytes", i,
			     plane_size);
			return result(audio);
		}
	}

	for (int i = 0; i < channels; ++i) {
		frame_datas[i].resize(fout->nb_samples);
		copy_n((float *)fout->extended_data[i], fout->nb_samples,
		       frame_datas[i].data());
		audio->data[i] = (uint8_t *)frame_datas[i].data();
	}
	audio->timestamp = av_rescale_q(
		fout->pts, av_buffersink_get_time_base(outputFilter_),
		AVRational{1, 1000000000});
	audio->frames = fout->nb_samples;

	return result(audio);
}

bool FFAFilter::obs_properties_add_avfilter(obs_properties *props,
					    obs_data *settings,
					    const obs_audio_info &oai,
					    const AVFilter *filter)
{
	auto filterOpts = FFAFilterOpts::Get(filter->name);
	if (!filterOpts)
		return false;

	filterOpts->AddToProperties(props, settings, oai);
	return true;
}

bool FFAFilter::obs_properties_hide_other_avclass(obs_properties *props,
						  const AVFilter *filter)
{
	auto cur_prop = obs_properties_first(props);
	do {
		if (!cur_prop)
			break;
		const char *prop_name = obs_property_name(cur_prop);
		if (!prop_name)
			continue;
		const char *slashpos = strchr(prop_name, '/');
		if (slashpos) {
			int name_len = slashpos - prop_name;
			if (strncmp(filter->name, prop_name, name_len) != 0)
				obs_property_set_visible(cur_prop, false);
		}

	} while (obs_property_next(&cur_prop));

	return true;
}

FFAFilter::FFAFilter(obs_data_t *settings, obs_source_t *filter)
	: source_(filter)
{
	update(settings);
}

void FFAFilter::update(obs_data_t *s)
{
	lock_guard<mutex> lg(graph_mutex_);
	next_filter_graph_.reset(new FilterGraph());
	obs_audio_info oai;
	obs_get_audio_info(&oai);
	next_filter_graph_->initFilter(s, &oai);
}

obs_audio_data *FFAFilter::filterAudio(obs_audio_data *audio)
{
	lock_guard<mutex> lg(graph_mutex_);
	if (!filter_graph_ && next_filter_graph_)
		filter_graph_.swap(next_filter_graph_);

	if (!filter_graph_)
		return audio;

	last_audio_data_ = *audio;

	auto result = filter_graph_->filterAudio(
		&last_audio_data_, last_audio_data_bufs_, !!next_filter_graph_);
	if (next_filter_graph_) {
		filter_graph_.swap(next_filter_graph_);
		next_filter_graph_.reset();
	}

	return result;
}

obs_properties_t *FFAFilter::properties()
{
	auto props = obs_properties_create();
	obs_properties_set_param(props, this, nullptr);

	obs_properties_add_button2(props, "export_clipboard",
				   "Export to clipboard",
				   ExportToClipboardClicked, this);
	obs_properties_add_button2(props, "import_clipboard",
				   "Import from clipboard",
				   ImportFromClipboardClicked, this);

	auto filterlist = obs_properties_add_list(
		props, "filter_name", obs_module_text("Filter"),
		obs_combo_type::OBS_COMBO_TYPE_LIST,
		obs_combo_format::OBS_COMBO_FORMAT_STRING);

	for (auto &x : FFAFilterCollection::Get())
		obs_property_list_add_string(filterlist, x.first.c_str(),
					     x.first.c_str());

	auto callback = [](obs_properties_t *props, obs_property_t *property,
			   obs_data_t *settings) -> bool {
		auto filterCollection = FFAFilterCollection::Get();
		FFAFilter *thiz = (FFAFilter *)obs_properties_get_param(props);
		const char *filtername =
			obs_data_get_string(settings, "filter_name");

		if (!filtername)
			return true;

		auto it = filterCollection.find(filtername);
		if (it == filterCollection.end())
			return true;

		auto &filter = it->second;

		obs_audio_info oai;
		obs_get_audio_info(&oai);

		obs_properties_hide_other_avclass(props, filter);
		obs_properties_add_avfilter(props, settings, oai, filter);

		thiz->current_filter_name_ = filtername;
		return true;
	};

	obs_property_set_modified_callback(filterlist, callback);
	return props;
}

bool FFAFilter::ExportToClipboardClicked(obs_properties_t *props,
					 obs_property_t *,
					 void *data)
{
	auto *thiz = static_cast<FFAFilter *>(data);
	obs_source_t *source = thiz ? thiz->source_ : nullptr;
	if (!source)
		return false;

	obs_data_t *settings = obs_source_get_settings(source);
	if (!settings)
		return false;

	obs_data_t *exported = obs_data_create();
	for (obs_property_t *p = obs_properties_first(props); p;
	     obs_property_next(&p)) {
		if (!obs_property_visible(p))
			continue;
		const char *name = obs_property_name(p);
		if (!name || is_default_value(settings, name))
			continue;
		copy_data_item(exported, settings, name);
	}

	const char *json = obs_data_get_json_pretty(exported);
	if (json && *json) {
		if (set_clipboard_text(json))
			blog(LOG_INFO, TAG "exported settings to clipboard");
		else
			blog(LOG_WARNING, TAG,
			     "failed to copy settings to clipboard");
	}

	obs_data_release(exported);
	obs_data_release(settings);
	return false;
}

bool FFAFilter::ImportFromClipboardClicked(obs_properties_t *props,
					   obs_property_t *,
					   void *data)
{
	auto *thiz = static_cast<FFAFilter *>(data);
	obs_source_t *source = thiz ? thiz->source_ : nullptr;
	if (!source)
		return false;

	std::string json = get_clipboard_text();
	if (json.empty()) {
		blog(LOG_WARNING, TAG, "clipboard has no text to import");
		return false;
	}

	obs_data_t *imported = obs_data_create_from_json(json.c_str());
	if (!imported) {
		blog(LOG_WARNING, TAG,
		     "failed to parse settings from clipboard");
		return false;
	}

	obs_data_t *settings = obs_source_get_settings(source);
	if (!settings) {
		obs_data_release(imported);
		return false;
	}

	obs_data_apply(settings, imported);
	blog(LOG_INFO, TAG, "imported settings from clipboard");

	// The per-filter options are only built by the filter_name modified
	// callback, so replay it to rebuild the option list for the imported
	// filter before returning true to refresh the UI.
	obs_properties_apply_settings(props, settings);

	obs_data_release(settings);
	obs_data_release(imported);
	return true;
}

extern "C" {
bool register_ffmepg_audio_filter()
{
#if LIBAVFILTER_VERSION_MAJOR < 7
	avfilter_register_all();
#endif
	if (FFAFilterCollection::Get().Init()) {
		RegisterFilter<FFAFilter>();
		return true;
	} else
		return false;
}

bool obs_module_load()
{
	if (avfilter_version() != LIBAVFILTER_VERSION_INT) {
		blog(LOG_ERROR, "%s", "[obs-ffmpeg-afilter] required " LIBAVFILTER_IDENT);
		return false;
	}

	auto log_callback = [](void *ud, int level, const char *fmt, va_list args) {
		using namespace std::chrono;
		static steady_clock::time_point log_window_start = steady_clock::now();
		static int log_count = 0;
		static std::mutex log_mutex;
		static std::atomic_bool too_many_logs = false;
		if (too_many_logs.load())
			return;
		blogva(LOG_WARNING, fmt, args);
		if (++log_count > 100) {
			if (log_mutex.try_lock()) {
				auto now = steady_clock::now();
				if (now - log_window_start < seconds(5)) { // output more than 100 lines of logs in 5 seconds
					blog(LOG_WARNING, "%s", "[obs-ffmpeg-afilter] too many logs. stop record.");
					too_many_logs.store(true);
				} else {
					log_window_start = now;
					log_count = 0;
				}
				log_mutex.unlock();
			}
		}
	};

	av_log_set_callback(log_callback);

	if (register_ffmepg_audio_filter())
		return true;
	else
		return false;
}

void obs_module_unload() {}

};
