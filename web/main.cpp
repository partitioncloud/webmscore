#include <emscripten/emscripten.h>

#include <QGuiApplication>
#include <QFontDatabase>
#include <QFile>
#include <QTemporaryFile>
#include "global/log.h"
#include "global/defer.h"
#include "global/io/buffer.h"
#include "global/io/file.h"

#include "modularity/ioc.h"
#include "context/internal/globalcontext.h"
#include "notation/internal/notationconfiguration.h"
#include "global/io/internal/filesystem.h"
#include "global/internal/cryptographichash.h"
#include "draw/drawmodule.h"
#include "engraving/engravingmodule.h"
#include "mpe/mpemodule.h"
#include "importexport/musicxml/musicxmlmodule.h"
#include "importexport/guitarpro/guitarpromodule.h"
#include "importexport/midi/midimodule.h"
#include "importexport/imagesexport/imagesexportmodule.h"
#include "importexport/imagesexport/iimagesexportconfiguration.h"

#include "draw/ifontprovider.h"
#include "engraving/dom/score.h"
#include "project/internal/notationproject.h"
#include "global/containers.h"
#include "engraving/engravingproject.h"
#include "engraving/compat/mscxcompat.h"
#include "project/internal/notationreadersregister.h"
#include "project/internal/notationwritersregister.h"
#include "engraving/dom/excerpt.h"
#include "engraving/editing/undo.h"
#include "engraving/editing/editexcerpt.h"
#include "engraving/engravingerrors.h"
#include "converter/internal/compat/notationmeta.h"
#include "notation/internal/notation.h"
#include "notation/internal/masternotation.h"
#include "notation/internal/mscnotationwriter.h"
#include "importexport/midi/internal/midiexport/exportmidi.h"
#include "./importexport/positionjsonwriter.h"

#include "./score.h"
#include "./wasmres.h"
#include "./audio/audio.h"

using namespace mu;
using namespace muse;
using project::INotationWriter;

//! NOTE Resolution of the exported PNG images, in dpi.
//! This used to be `mu::engraving::DPI` (which was 360 until MuseScore 4.7)
static constexpr float PNG_DPI_RESOLUTION = 360.f;

static auto s_globalContext = std::make_shared<context::GlobalContext>();

using NotationProjectPtr = std::shared_ptr<project::NotationProject>;

//! The loaded scores. The `NotationProject` owns the `EngravingProject` (which owns the
//! `MasterScore`) and the `MasterNotation` the playback and the audio writers need.
static std::map<engraving::MasterScore*, NotationProjectPtr> s_projects;

static NotationProjectPtr projectOf(engraving::MasterScore* score) {
    return muse::value(s_projects, score->masterScore());
}

/**
 * The playback (the audio engine sequence) follows the current notation of the global context:
 * make `score` the current one before any audio operation.
 * Switching rebuilds the sequence, its output params come from the project's audio settings.
 */
static void makeCurrent(engraving::MasterScore* score) {
    NotationProjectPtr proj = projectOf(score);
    if (proj && s_globalContext->currentProject() != proj) {
        s_globalContext->setCurrentProject(proj);
    }
}

static void loadFonts();

/**
 * MSCZ/MSCX file format version
 */
int _version() {
    return engraving::Constants::MSC_VERSION;
}

/**
 * init libmscore
 */
void _init(int argc, char** argv) {
    setenv("QT_QPA_PLATFORM", "offscreen", 1); // https://stackoverflow.com/a/70978934
    setenv("QT_QPA_FONTDIR", "/fonts", 1);
    new QGuiApplication(argc, argv);
    loadFonts();

    modularity::ioc()->registerExport<context::IGlobalContext>("", s_globalContext); // context interface, not a global one
    auto notationConfiguration = new notation::NotationConfiguration();
    modularity::globalIoc()->registerExport<notation::INotationConfiguration>("", notationConfiguration);
    // `NotationConfiguration::init()` is never called here, so the setting has no default
    // value and reads as `false`: the synthesized audio would skip repeats
    notationConfiguration->setIsPlayRepeatsEnabled(true);

    // src/framework/global/globalmodule.cpp#67
    modularity::globalIoc()->registerExport<io::IFileSystem>("", new io::FileSystem());
    modularity::globalIoc()->registerExport<ICryptographicHash>("", new CryptographicHash());

    // src/framework/draw/drawmodule.cpp
    auto drawM = new draw::DrawModule();
    drawM->registerExports();

    auto engM = new engraving::EngravingModule();
    engM->registerExports();
    engM->onInit(IApplication::RunMode::ConsoleApp);
    auto mpeM = new mpe::MpeModule();
    mpeM->registerExports();

    // populate `engraving::instrumentGroups` and `engraving::instrumentTemplates`
    engraving::clearInstrumentTemplates();
    engraving::loadInstrumentTemplates("/instruments.xml");

    // file import/export
    modularity::ioc()->registerExport<project::INotationReadersRegister>("", new project::NotationReadersRegister());
    modularity::ioc()->registerExport<project::INotationWritersRegister>("", new project::NotationWritersRegister());
    auto mxlM = new iex::musicxml::MusicXmlModule();
    mxlM->registerExports();
    mxlM->resolveImports();
    auto gpM = new iex::guitarpro::GuitarProModule();
    gpM->registerExports();
    gpM->resolveImports();
    auto midiM = new iex::midi::MidiModule();
    midiM->registerExports();
    midiM->resolveImports();
    midiM->onInit(IApplication::RunMode::ConsoleApp);
    auto imgM = new iex::imagesexport::ImagesExportModule();
    imgM->registerExports();
    imgM->resolveImports();
    imgM->onInit(IApplication::RunMode::ConsoleApp);

    // The PNG writer exports at `mu::engraving::DPI`, which changed from 360 to 1200 in
    // MuseScore 4.7 (an A4 page would become 9924x14028 px). Keep the previous size.
    auto imgConf = modularity::ioc()->resolve<iex::imagesexport::IImagesExportConfiguration>("");
    imgConf->setExportPngDpiResolutionOverride(PNG_DPI_RESOLUTION);

    auto writers = modularity::ioc()->resolve<project::INotationWritersRegister>("");
    writers->reg({ engraving::MSCZ }, std::make_shared<notation::MscNotationWriter>(engraving::MscIoMode::Zip));
    // writers->reg({ engraving::MSCX }, std::make_shared<notation::MscNotationWriter>(engraving::MscIoMode::Dir));
    writers->reg({ engraving::MSCS }, std::make_shared<notation::MscNotationWriter>(engraving::MscIoMode::XmlFile));

    MainAudio::initModule();
}

/**
 * load (CJK) fonts on demand
 */
bool _addFont(const char* fontPath) {
    String _fontPath = String::fromUtf8(fontPath);
    if (-1 == QFontDatabase::addApplicationFont(QString::fromUtf8(fontPath))) {
        LOGE() << String(u"Cannot load font <%1>").arg(_fontPath);
        return false;
    } else {
        return true;
    }
}

/**
 * Loads the embedded fonts explicitely
 * (setting QT_QPA_FONTDIR to /fonts doesn't seem to be enough)
 */
static void loadFonts() {
    const char* embeddedFonts[] = {
        "/fonts/bravura/BravuraText.woff2",
        "/fonts/bravura/Bravura.woff2",
        "/fonts/campania/Campania.woff2",
        "/fonts/edwin/Edwin-BdIta.woff2",
        "/fonts/edwin/Edwin-Bold.woff2",
        "/fonts/edwin/Edwin-Italic.woff2",
        "/fonts/edwin/Edwin-Roman.woff2",
        "/fonts/finalebroadway/FinaleBroadwayText.woff2",
        "/fonts/finalebroadway/FinaleBroadway.woff2",
        "/fonts/finalemaestro/FinaleMaestroText.woff2",
        "/fonts/finalemaestro/FinaleMaestro.woff2",
        "/fonts/FreeSans.woff2",
        "/fonts/FreeSerifBoldItalic.woff2",
        "/fonts/FreeSerifBold.woff2",
        "/fonts/FreeSerifItalic.woff2",
        "/fonts/FreeSerif.woff2",
        "/fonts/gootville/GootvilleText.woff2",
        "/fonts/gootville/Gootville.woff2",
        "/fonts/leland/LelandText.woff2",
        "/fonts/leland/Leland.woff2",
        "/fonts/mscore-BC.woff2",
        "/fonts/mscore/MScoreText.woff2",
        "/fonts/mscore/MScore.woff2",
        "/fonts/mscoreTab.woff2",
        "/fonts/musejazz/MuseJazzText.woff2",
        "/fonts/musejazz/MuseJazz.woff2",
        "/fonts/petaluma/PetalumaScript.woff2",
        "/fonts/petaluma/PetalumaText.woff2",
        "/fonts/petaluma/Petaluma.woff2",
    };

    int missedCount = 0;
    int totalCount = (sizeof(embeddedFonts) / sizeof(embeddedFonts[0]));
    for (const char* fontPath : embeddedFonts) {
        if (!_addFont(fontPath))
            missedCount++;
    }

    LOGI() << "Fonts loaded: " << totalCount - missedCount << "/" << totalCount;
}

/**
 * Load MSCX/MSCZ
 * https://github.com/LibreScore/webmscore/blob/v4.0/src/project/internal/notationproject.cpp#L187-L223
 */
Ret _doLoad(engraving::EngravingProjectPtr proj, QString filePath, bool doLayout) {
    // read score using the `compat` method
    engraving::Err err = static_cast<engraving::Err>(engraving::compat::loadMsczOrMscx(proj->masterScore(), filePath, true).code());
    if (err != engraving::Err::NoError) {
        return make_ret(err);
    }

    engraving::MasterScore* score = proj->masterScore();
    IF_ASSERT_FAILED(score) {
        return make_ret(engraving::Err::UnknownError);
    }

    // make `score->update()` in `doSetupMasterScore` have no effect,
    // so that we could "do layout" later
    score->lockUpdates(true);
    DEFER {
        score->lockUpdates(false);
    };

    // Setup master score
    err = static_cast<engraving::Err>(proj->setupMasterScore(true).code());
    if (err != engraving::Err::NoError) {
        return make_ret(err);
    }

    // do layout ...
    score->lockUpdates(false);
    if (doLayout) {
        score->setLayoutAll(); // FIXME: 
        score->update();
        score->switchToPageMode();  // the default _layoutMode is LayoutMode::PAGE, but the score file may be saved in continuous mode
    }

    return make_ok();
}

/**
 * Load other file formats
 * https://github.com/LibreScore/webmscore/blob/v4.0/src/project/internal/notationproject.cpp#L246-L291
 */
Ret _doImport(engraving::EngravingProjectPtr proj, QString filePath, bool doLayout) {
    // Find import reader
    std::string suffix = io::suffix(filePath.toStdString());
    auto readers = modularity::ioc()->resolve<project::INotationReadersRegister>("");
    project::INotationReaderPtr scoreReader = readers->reader(suffix);
    if (!scoreReader) {
        return make_ret(engraving::Err::FileUnknownType);
    }

    // Setup import reader
    project::INotationReader::Options options;
    options[project::INotationReader::OptionKey::ForceMode] = Val(true);

    // Read(import) master score
    engraving::MasterScore* score = proj->masterScore();
    Ret ret = scoreReader->read(score, filePath, options);
    if (!ret.success()) {
        return ret;
    }

    // post-processing for non-native formats
    score->setMetaTag(u"originalFormat", QString::fromStdString(suffix));
    score->connectTies(); // HACK: ???

    if (!doLayout) {
        // make `score->update()` in `doSetupMasterScore` have no effect
        score->lockUpdates(true);
        DEFER {
            score->lockUpdates(false);
        };
    }

    // Setup master score
    engraving::Err err = static_cast<engraving::Err>(proj->setupMasterScore(true).code());
    if (err != engraving::Err::NoError) {
        return make_ret(err);
    }

    return make_ok();
}

/**
 * load score
 */
WasmRes _load(const char* format, const char* data, const uint32_t size, bool doLayout) {
    String _format = String::fromUtf8(format);  // file format of the data

    // create a temporary file, and write `data` into it
    QTemporaryFile tempfile("XXXXXX." + _format);  // filename template for the temporary file
    if (!tempfile.open()) { // a QTemporaryFile will always be opened in `QIODevice::ReadWrite` mode
        throw QString("Cannot create a temporary file");
    } else {
        tempfile.write(data, size);
        tempfile.close(); // calls QFileDevice::flush() and closes the file
    }
    QString filePath = tempfile.fileName(); // temporary filename
    DEFER {
        // delete the temporary file
        tempfile.remove();
    };

    // create notation & engraving project
    auto notationProj = std::make_shared<project::NotationProject>(modularity::globalCtx());
    notationProj->setupProject();
    notationProj->setPath(filePath);

    auto proj = notationProj->m_engravingProject;
    // // `MasterScore::name()` requires a `FileInfoProvider` to get the file name, etc.
    // proj->setFileInfoProvider(std::make_shared<engraving::LocalFileInfoProvider>(filePath));
    
    // do load
    Ret ret = engraving::isMuseScoreFile(format)
        ? _doLoad(proj, filePath, doLayout)
        : _doImport(proj, filePath, doLayout);

    // handle exceptions
    if (!ret.success()) {
        return WasmRes::fromRet(ret);
    }

    engraving::MasterScore* score = proj->masterScore();
    notationProj->m_masterNotation->setMasterScore(score);
    s_projects[score] = notationProj;

    // Set project only after master score is attached
    s_globalContext->setCurrentProject(notationProj);

    auto score_ptr = reinterpret_cast<uintptr_t>(score);
    return WasmRes(score_ptr);
}

/**
 * Generate excerpts from Parts (only parts that are visible) if no existing excerpts
 */
void _generateExcerpts(uintptr_t score_ptr) {
    MainScore score(score_ptr);

    auto scoreExcerpts = score->excerpts();
    if (scoreExcerpts.size() > 0) {
        // has existing excerpts
        return;
    }

    auto parts = score->parts();
    auto excerpts = engraving::Excerpt::createExcerptsFromParts(parts, score->masterScore());

    // TODO: testing
    // https://github.com/LibreScore/webmscore/blob/v4.0/src/engraving/libmscore/unrollrepeats.cpp#L99-L117
    for (auto e : excerpts) {
        engraving::Score* nscore = e->masterScore()->createScore();
        e->setExcerptScore(nscore);
        nscore->style().set(engraving::Sid::createMultiMeasureRests, true);
        auto excerptCmdFake = new engraving::AddExcerpt(e);
        excerptCmdFake->redo(nullptr);
        engraving::Excerpt::createExcerpt(e);

        // add this excerpt back to the score excerpt list
        scoreExcerpts.push_back(e);
    }

    LOGI() << String(u"Generated excerpts: size %1").arg((int)excerpts.size());
}

/**
 * get the score title
 */
WasmRes _title(uintptr_t score_ptr) {
    MainScore score(score_ptr);
    // https://github.com/LibreScore/webmscore/blob/v4.0/src/converter/internal/compat/notationmeta.cpp#L89-L107
    String title = converter::NotationMeta::title(score);
    return WasmRes(title);
}

/**
 * get the number of pages
 */
WasmRes _npages(uintptr_t score_ptr, int excerptId) {
    MainScore score(score_ptr, excerptId);
    return WasmRes(score->npages());
}

/**
 * Export score file using one of the `NotationWriter`s
 * https://github.com/LibreScore/webmscore/blob/v4.0/src/converter/internal/compat/backendapi.cpp#L465-L491
 */
Ret processWriter(String writerName, engraving::MasterScore * score, io::IODevice& device, const INotationWriter::Options& options = INotationWriter::Options()) {
    // Find file writer
    auto writers = modularity::ioc()->resolve<project::INotationWritersRegister>("");
    auto writer = writers->writer(writerName.toStdString());
    if (!writer) {
        LOGE() << "Not found writer " << writerName;
        return make_ret(Ret::Code::InternalError);
    }

    // FIXME: persist this `Notation` object
    // NOTE The master notation is needed by the writers that use the playback (audio export)
    NotationProjectPtr proj = projectOf(score);
    if (!proj) {
        return make_ret(Ret::Code::InternalError);
    }
    auto masterNotation = std::dynamic_pointer_cast<notation::MasterNotation>(proj->masterNotation());
    auto notation = std::make_shared<notation::Notation>(masterNotation.get(), modularity::ContextPtr(), score);

    // Write
    Ret writeRet = writer->write(notation, device, options);
    if (!writeRet) {
        LOGE() << writeRet.toString();
        return writeRet;
    }

    return make_ok();

}

Ret processWriter(String writerName, engraving::MasterScore * score, QByteArray* buffer, const INotationWriter::Options& options = INotationWriter::Options()) {
    ByteArray ioData;
    io::Buffer device(&ioData);
    device.open(io::IODevice::ReadWrite);
    DEFER {
        device.close();
    };

    Ret ret = processWriter(writerName, score, device, options);
    if (ret.success()) {
        *buffer = ioData.toQByteArray();
    }

    return ret;
}

/**
 * export score as MusicXML file
 */
WasmRes _saveXml(uintptr_t score_ptr, int excerptId) {
    MainScore score(score_ptr, excerptId);

    QByteArray data;
    processWriter(u"xml", score, &data);
    LOGI() << String(u"excerpt %1, size %2 bytes").arg(excerptId, data.size());

    return WasmRes(data);
}

/**
 * export score as compressed MusicXML file
 */
WasmRes _saveMxl(uintptr_t score_ptr, int excerptId) {
    MainScore score(score_ptr, excerptId);

    QByteArray data;
    processWriter(u"mxl", score, &data);
    LOGI() << String(u"excerpt %1, size %2 bytes").arg(excerptId, data.size());

    return WasmRes(data);
}

/**
 * save part score as MSCZ/MSCX file
 */
WasmRes _saveMsc(uintptr_t score_ptr, bool compressed, int excerptId) {
    MainScore score(score_ptr, excerptId);

    if (!score->isMaster()) {  // clone metaTags from masterScore
        auto j(score->masterScore()->metaTags());
        for (auto p : j) {
            if (p.first != "partName")  // don't copy "partName" should that exist in masterScore
                score->metaTags().insert({p.first, p.second});
            score->metaTags().insert({u"platform", u"webmscore"});
            score->metaTags().insert({u"source", u"https://github.com/LibreScore/webmscore"});
            score->metaTags().insert({u"creationDate", Date::currentDate().toString()});  // update "creationDate"
        }
    }

    QByteArray data;
    Ret ret = processWriter(u"mscz", score, &data);
    if (!ret.success()) {
        return WasmRes::fromRet(ret);
    }

    if (!compressed) {
        // HACK: read the .mscx file inside mscz
        // In MuseScore 4, the so-called "mscx" is exported as a directory
        io::Buffer msczBuf((const uint8_t*)data.constData(), data.size());
        engraving::MscReader::Params params;
        params.device = &msczBuf;
        params.mode = engraving::MscIoMode::Zip;

        engraving::MscReader reader(params);
        reader.open();
        data = reader.readScoreFile().toQByteArray(); // can't use `NoCopy` here because `msczBuf` is destroyed as the code block ends
    }

    if (!score->isMaster()) {  // remove metaTags added above
        auto j(score->masterScore()->metaTags());
        for (auto p : j) {
            // remove all but "partName", should that exist in masterScore
            if (p.first != "partName")
                score->metaTags().erase(p.first);
        }
    }

    LOGI() << String(u"compressed %1, excerpt %2, size %3").arg(compressed, excerptId, data.size());
    return WasmRes(data);
}

/**
 * export score as SVG
 */
WasmRes _saveSvg(uintptr_t score_ptr, int pageNumber, bool drawPageBackground, int excerptId) {
    MainScore score(score_ptr, excerptId);

    // config
    score->switchToPageMode();
    INotationWriter::Options options {
        { INotationWriter::OptionKey::PAGE_NUMBER, Val(pageNumber) },
        { INotationWriter::OptionKey::TRANSPARENT_BACKGROUND, Val(!drawPageBackground) },
        // { INotationWriter::OptionKey::BEATS_COLORS, Val::fromQVariant(beatsColors) }
    };

    QByteArray data;
    Ret ret = processWriter(u"svg", score, &data, options);
    LOGI() << String(u"excerpt %1, page index %2, size %3 bytes").arg(excerptId, pageNumber, data.size());
    if (!ret.success()) {
        return WasmRes::fromRet(ret);
    }

    return WasmRes(data);
}

/**
 * export score as PNG
 */
WasmRes _savePng(uintptr_t score_ptr, int pageNumber, bool drawPageBackground, bool transparent, int excerptId) {
    MainScore score(score_ptr, excerptId);

    // config
    score->switchToPageMode();
    INotationWriter::Options options {
        { INotationWriter::OptionKey::PAGE_NUMBER, Val(pageNumber) },
        { INotationWriter::OptionKey::TRANSPARENT_BACKGROUND, Val(!drawPageBackground) },
    };

    QByteArray data;
    Ret ret = processWriter(u"png", score, &data, options);
    LOGI() << String(u"excerpt %1, page index %2, drawPageBackground %3, transparent %4, size %5 bytes").arg(excerptId).arg(pageNumber).arg(drawPageBackground).arg(transparent).arg(data.size());
    if (!ret.success()) {
        return WasmRes::fromRet(ret);
    }

    return WasmRes(data);
}

/**
 * export score as PDF
 */
WasmRes _savePdf(uintptr_t score_ptr, int excerptId) {
    MainScore score(score_ptr, excerptId);

    INotationWriter::Options options;
    // options[INotationWriter::OptionKey::UNIT_TYPE] = Val(INotationWriter::UnitType::MULTI_PART);

    QByteArray data;
    Ret ret = processWriter(u"pdf", score, &data, options);
    LOGI() << String(u"excerpt %1, size %2 bytes").arg(excerptId, data.size());
    if (!ret.success()) {
        return WasmRes::fromRet(ret);
    }

    return WasmRes(data);
}

/**
 * export score as MIDI
 */
WasmRes _saveMidi(uintptr_t score_ptr, bool midiExpandRepeats, bool exportRPNs, int excerptId) {
    MainScore score(score_ptr, excerptId);

    QBuffer buffer;
    buffer.open(QIODevice::ReadWrite);

    // TODO: refactor to `INotationWriter`
    // use `exportMidi.write` directly
    // https://github.com/LibreScore/webmscore/blob/v4.0/src/importexport/midi/internal/notationmidiwriter.cpp#L57-L64
    iex::midi::ExportMidi exportMidi(score);
    auto synthesizerState = score->synthesizerState();
    exportMidi.write(&buffer, midiExpandRepeats, exportRPNs, synthesizerState);

    int size = buffer.size();
    LOGI() << String(u"excerpt %1, midiExpandRepeats %2, exportRPNs %3, size %4").arg(excerptId).arg(midiExpandRepeats).arg(exportRPNs).arg(size);

    return WasmRes(buffer.data());
}

/**
 * export score as AudioFile (wav/ogg)
 */
WasmRes _saveAudio(uintptr_t score_ptr, const char* format, int excerptId) {
    MainScore score(score_ptr, excerptId);

    // file format of the output file
    // "wav", "ogg", "flac", or "mp3"
    QString _format = QString::fromUtf8(format);
    if (!(_format == "wav" || _format == "ogg" || _format == "flac" || _format == "mp3")) {
        throw QString("Invalid output format");
    }

    // Audio export requires a real output path via IODevice meta("file_path").
    QTemporaryFile tempfile("XXXXXX." + _format);
    if (!tempfile.open()) {
        throw QString("Cannot create a temporary file");
    }
    QString filePath = tempfile.fileName();
    tempfile.close();

    DEFER {
        // delete the temporary file
        tempfile.remove();
    };

    makeCurrent(score);

    io::File outputFile(filePath.toStdString());
    if (!outputFile.open(io::IODevice::WriteOnly)) {
        return WasmRes::fromRet(make_ret(Ret::Code::InternalError));
    }

    outputFile.setMeta("file_path", filePath.toStdString());
    Ret ret = processWriter(_format, score, outputFile);
    outputFile.close();
    if (!ret.success()) {
        return WasmRes::fromRet(ret);
    }

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return WasmRes::fromRet(make_ret(Ret::Code::InternalError));
    }

    QByteArray data = file.readAll();
    LOGI() << String(u"excerpt %1, size %2 bytes").arg(excerptId, data.size());

    return WasmRes(data);
}

/**
 * save positions of measures or segments (if the `ofSegments` param == true) as JSON
 */
WasmRes _savePositions(uintptr_t score_ptr, bool ofSegments, int excerptId) {
    MainScore score(score_ptr, excerptId);
    score->switchToPageMode();

    using W = notation::PositionJsonWriter;
    W writer(ofSegments ? W::ElementType::SEGMENT : W::ElementType::MEASURE);
    
    QByteArray data = writer.jsonData(score);
    LOGI() << String(u"excerpt %1, ofSegments %2, file size %3").arg(excerptId).arg(ofSegments).arg(data.size());

    return WasmRes(data);
}

/**
 * save score metadata as JSON
 */
WasmRes _saveMetadata(uintptr_t score_ptr) {
    MainScore score(score_ptr);
    auto result = converter::NotationMeta::metaJson(score);
    auto data = result.val;
    return WasmRes(
        ByteArray(data.data(), data.size()) // UTF-8 encoded JSON data
    );
}

/**
 * export functions (can only be C functions)
 */
extern "C" {

    EMSCRIPTEN_KEEPALIVE
    int version() {
        return _version();
    };

    EMSCRIPTEN_KEEPALIVE
    void setLogLevel(const logger::Level level) {
        logger::Logger::instance()->setLevel(level);
    };

    EMSCRIPTEN_KEEPALIVE
    void init(int argc, char** argv) {
        return _init(argc, argv);
    };

    EMSCRIPTEN_KEEPALIVE
    bool addFont(const char* fontPath) {
        return _addFont(fontPath);
    };

    EMSCRIPTEN_KEEPALIVE
    WasmResBytes load(const char* format, const char* data, const uint32_t size, bool doLayout = true) {
        return _load(format, data, size, doLayout);
    };

    EMSCRIPTEN_KEEPALIVE
    void generateExcerpts(uintptr_t score_ptr) {
        return _generateExcerpts(score_ptr);
    };

    EMSCRIPTEN_KEEPALIVE
    WasmResBytes title(uintptr_t score_ptr) {
        return _title(score_ptr);
    };

    EMSCRIPTEN_KEEPALIVE
    WasmResBytes npages(uintptr_t score_ptr, int excerptId) {
        return _npages(score_ptr, excerptId);
    };

    EMSCRIPTEN_KEEPALIVE
    WasmResBytes saveXml(uintptr_t score_ptr, int excerptId = -1) {
        return _saveXml(score_ptr, excerptId);
    };

    EMSCRIPTEN_KEEPALIVE
    WasmResBytes saveMxl(uintptr_t score_ptr, int excerptId = -1) {
        return _saveMxl(score_ptr, excerptId);
    };

    EMSCRIPTEN_KEEPALIVE
    WasmResBytes saveMsc(uintptr_t score_ptr, bool compressed, int excerptId = -1) {
        return _saveMsc(score_ptr, compressed, excerptId);
    };

    EMSCRIPTEN_KEEPALIVE
    WasmResBytes saveSvg(uintptr_t score_ptr, int pageNumber, bool drawPageBackground, int excerptId = -1) {
        return _saveSvg(score_ptr, pageNumber, drawPageBackground, excerptId);
    };

    EMSCRIPTEN_KEEPALIVE
    WasmResBytes savePng(uintptr_t score_ptr, int pageNumber, bool drawPageBackground, bool transparent, int excerptId = -1) {
        return _savePng(score_ptr, pageNumber, drawPageBackground, transparent, excerptId);
    };

    EMSCRIPTEN_KEEPALIVE
    WasmResBytes savePdf(uintptr_t score_ptr, int excerptId = -1) {
        return _savePdf(score_ptr, excerptId);
    };

    EMSCRIPTEN_KEEPALIVE
    WasmResBytes saveMidi(uintptr_t score_ptr, bool midiExpandRepeats, bool exportRPNs, int excerptId = -1) {
        return _saveMidi(score_ptr, midiExpandRepeats, exportRPNs, excerptId);
    };

    EMSCRIPTEN_KEEPALIVE
    WasmResBytes saveAudio(uintptr_t score_ptr, const char* format, int excerptId = -1) {
        return _saveAudio(score_ptr, format, excerptId);
    };

    EMSCRIPTEN_KEEPALIVE
    WasmResBytes getAudioOutputParams(uintptr_t score_ptr, int excerptId = -1) {
        MainScore score(score_ptr, excerptId);
        makeCurrent(score);
        return WasmRes::fromRet(MainAudio::getOutputParams(score));
    }

    EMSCRIPTEN_KEEPALIVE
    WasmResBytes setAudioOutputParams(uintptr_t score_ptr, const char* json, int excerptId = -1) {
        MainScore score(score_ptr, excerptId);
        makeCurrent(score);
        return WasmRes::fromRet(MainAudio::setOutputParams(score, json));
    }

    EMSCRIPTEN_KEEPALIVE
    uintptr_t synthAudio(uintptr_t score_ptr, float starttime, int excerptId = -1) {
        MainScore score(score_ptr, excerptId);
        makeCurrent(score);
        return MainAudio::Synth::start(score, starttime);
    };

    EMSCRIPTEN_KEEPALIVE
    const char* processSynth(uintptr_t fn_ptr, bool cancel = false) {
        return MainAudio::Synth(fn_ptr).process(cancel);
    }

    EMSCRIPTEN_KEEPALIVE
    const char* processSynthBatch(uintptr_t fn_ptr, int batchSize, bool cancel = false) {
        return MainAudio::Synth(fn_ptr).processBatch(batchSize, cancel);
    }

    EMSCRIPTEN_KEEPALIVE
    WasmResBytes savePositions(uintptr_t score_ptr, bool ofSegments, int excerptId = -1) {
        return _savePositions(score_ptr, ofSegments, excerptId);
    };

    EMSCRIPTEN_KEEPALIVE
    WasmResBytes saveMetadata(uintptr_t score_ptr) {
        return _saveMetadata(score_ptr);
    };

    EMSCRIPTEN_KEEPALIVE
    void destroy(uintptr_t score_ptr) {
        auto score = reinterpret_cast<engraving::MasterScore*>(score_ptr);
        NotationProjectPtr proj = projectOf(score);
        if (proj && s_globalContext->currentProject() == proj) {
            s_globalContext->setCurrentProject(nullptr);
        }
        // remove the only alive reference to the smart pointer:
        // destroying the `EngravingProject` also destroys its `MasterScore` in its destructor
        s_projects.erase(score);
    };

}
