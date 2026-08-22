#include "frame_sequence.h"

#include "png_writer.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace osss {
namespace {

double LumaOf(const std::uint32_t pixel) {
    return 0.2126 * static_cast<double>((pixel >> 16U) & 0xFFU) +
        0.7152 * static_cast<double>((pixel >> 8U) & 0xFFU) +
        0.0722 * static_cast<double>(pixel & 0xFFU);
}

std::uint32_t Pack(const double red, const double green, const double blue) {
    const auto clamp = [](const double value) {
        return static_cast<std::uint32_t>(std::lround(std::clamp(value, 0.0, 255.0)));
    };
    return 0xFF000000U | (clamp(red) << 16U) | (clamp(green) << 8U) | clamp(blue);
}

std::string JsonEscape(const std::string& text) {
    std::string escaped;
    escaped.reserve(text.size() + 8);
    for (const char character : text) {
        switch (character) {
        case '"':
            escaped += "\\\"";
            break;
        case '\\':
            escaped += "\\\\";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\t':
            escaped += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(character) < 0x20) {
                std::ostringstream stream;
                stream << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                       << static_cast<int>(character);
                escaped += stream.str();
            } else {
                escaped += character;
            }
            break;
        }
    }
    return escaped;
}

std::string Number(const double value, const int precision = 4) {
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(precision) << value;
    return stream.str();
}

std::string Padded(const int index) {
    std::ostringstream stream;
    stream << std::setw(3) << std::setfill('0') << index;
    return stream.str();
}

// The viewer, in two halves with the frame index injected between them.
//
// Emitted as a fragment -- a title, a style block, markup, a script -- rather
// than a whole document. A browser renders that verbatim from disk, and it is
// also the shape a published artifact wants, so one generator serves both
// without a second template drifting out of sync.
constexpr char kViewerHead[] = R"HTML(<title>OSSS Frame Stepper</title>
<style>
/* Neutrals carry a slight indigo bias toward the accent so the chrome reads as
   chosen rather than default grey. The accent is deliberately violet-leaning
   rather than cyan: the frames under inspection are full of cyan, magenta and
   yellow, and an accent that lands inside the test pattern's own palette would
   be read as content. For the same reason --warn sits apart from the red-to-
   white heat ramp the flicker map uses. */
:root {
  --surface: #f3f4f8;
  --panel: #ffffff;
  --ink: #16181f;
  --muted: #5b6070;
  --line: #d7d9e2;
  --accent: #4b53c8;
  --warn: #a32d46;
  --mount: #e3e5ec;
}
@media (prefers-color-scheme: dark) {
  :root:not([data-theme="light"]) {
    --surface: #0e1015;
    --panel: #171a22;
    --ink: #e5e7ef;
    --muted: #929aad;
    --line: #2a2f3c;
    --accent: #8b91f5;
    --warn: #ff8098;
    --mount: #05060a;
  }
}
:root[data-theme="dark"] {
  --surface: #0e1015;
  --panel: #171a22;
  --ink: #e5e7ef;
  --muted: #929aad;
  --line: #2a2f3c;
  --accent: #8b91f5;
  --warn: #ff8098;
  --mount: #05060a;
}
body {
  margin: 0;
  background: var(--surface);
  color: var(--ink);
  font: 13px/1.5 ui-monospace, SFMono-Regular, Consolas, monospace;
}
header { padding: 14px 18px 10px; border-bottom: 1px solid var(--line); }
h1 { margin: 0 0 2px; font-size: 15px; font-weight: 600; letter-spacing: .01em; }
.sub { color: var(--muted); font-size: 12px; }
main { display: grid; grid-template-columns: minmax(0, 1fr) 300px; gap: 14px; padding: 14px 18px 28px; }
@media (max-width: 900px) { main { grid-template-columns: minmax(0, 1fr); } }
.panel { background: var(--panel); border: 1px solid var(--line); border-radius: 6px; padding: 12px; }
.row { display: flex; flex-wrap: wrap; gap: 8px; align-items: center; margin-bottom: 10px; }
button, select, input[type="number"] {
  font: inherit; color: var(--ink); background: var(--panel);
  border: 1px solid var(--line); border-radius: 4px; padding: 3px 9px; cursor: pointer;
}
button:hover, select:hover { border-color: var(--accent); }
button[aria-pressed="true"] { background: var(--accent); border-color: var(--accent); color: #fff; }
:focus-visible { outline: 2px solid var(--accent); outline-offset: 2px; }
input[type="range"] { flex: 1 1 220px; min-width: 140px; accent-color: var(--accent); }
label { color: var(--muted); }
.stage { background: var(--mount); border: 1px solid var(--line); border-radius: 4px; overflow: auto; max-height: 72vh; }
.stage canvas { display: block; margin: 0 auto; image-rendering: pixelated; }
#sheet { display: none; grid-template-columns: repeat(auto-fill, minmax(150px, 1fr)); gap: 6px; }
#sheet figure { margin: 0; cursor: pointer; }
#sheet img { width: 100%; display: block; border: 1px solid var(--line); border-radius: 3px; }
#sheet figcaption { color: var(--muted); font-size: 11px; padding-top: 2px; }
#sheet figure[data-current="1"] img { border-color: var(--accent); outline: 1px solid var(--accent); }
table { border-collapse: collapse; width: 100%; font-size: 12px; font-variant-numeric: tabular-nums; }
th, td { text-align: right; padding: 3px 4px; border-bottom: 1px solid var(--line); }
th:first-child, td:first-child { text-align: left; }
th { color: var(--muted); font-weight: 500; }
td.hot { color: var(--warn); font-weight: 600; }
.spark { width: 100%; height: 64px; display: block; }
.keys { color: var(--muted); font-size: 11px; margin-top: 10px; }
.readout { color: var(--muted); font-size: 12px; min-height: 18px; font-variant-numeric: tabular-nums; }
/* The frame label is read while stepping, so its digits must not shift width. */
#frameLabel { font-variant-numeric: tabular-nums; }
</style>
<header>
  <h1 id="title">OSSS frame sequence</h1>
  <div class="sub" id="subtitle"></div>
</header>
<main>
  <section class="panel">
    <div class="row">
      <label for="sequence">sequence</label>
      <select id="sequence"></select>
      <button id="play" aria-pressed="false">play</button>
      <button id="stepBack">&#8592;</button>
      <button id="stepForward">&#8594;</button>
      <label for="fps">fps</label>
      <input id="fps" type="number" min="1" max="60" step="1" value="8" style="width:60px">
      <button id="loop" aria-pressed="true">loop</button>
      <label for="zoom">zoom</label>
      <select id="zoom">
        <option value="0.5">50%</option>
        <option value="1" selected>100%</option>
        <option value="2">200%</option>
        <option value="4">400%</option>
      </select>
      <button id="sheetToggle" aria-pressed="false">contact sheet</button>
    </div>
    <div class="row">
      <input id="frame" type="range" min="0" max="0" value="0">
      <span id="frameLabel" class="sub"></span>
    </div>
    <div class="row" id="views"></div>
    <div class="row">
      <button id="blink" aria-pressed="false">blink A/B</button>
      <label for="blinkView">against</label>
      <select id="blinkView"></select>
      <label for="blinkMs">ms</label>
      <input id="blinkMs" type="number" min="60" max="2000" step="20" value="320" style="width:70px">
    </div>
    <div class="stage" id="stage"><canvas id="canvas"></canvas></div>
    <div id="sheet"></div>
    <div class="readout" id="readout"></div>
    <div class="keys">left/right step &#183; space play &#183; b blink &#183; g contact sheet &#183; 1-9 view &#183; w jump to the worst step</div>
  </section>
  <aside class="panel">
    <div class="sub" style="margin-bottom:6px">error-step per lane, this frame</div>
    <table id="metrics"><thead><tr><th>lane</th><th>step</th><th>xfade</th><th>worst</th><th>at</th></tr></thead><tbody></tbody></table>
    <div class="sub" style="margin:12px 0 4px">error-step over the run</div>
    <canvas class="spark" id="spark"></canvas>
    <div class="sub" id="sparkLabel"></div>
  </aside>
</main>
<script>
const DATA = )HTML";

constexpr char kViewerTail[] = R"HTML(;

const $ = (id) => document.getElementById(id);
const state = {
  sequence: 0,
  frame: 0,
  view: 0,
  blinkView: 0,
  playing: false,
  blinking: false,
  blinkPhase: false,
  sheet: false,
  images: new Map(),
};

const sequences = DATA.sequences;
$("title").textContent = DATA.title;

function current() { return sequences[state.sequence]; }
function frames() { return current().frames; }
function viewNames() {
  const first = frames()[0];
  return first ? Object.keys(first.views) : [];
}

function imageFor(url) {
  let image = state.images.get(url);
  if (!image) {
    image = new Image();
    image.src = url;
    state.images.set(url, image);
  }
  return image;
}

// Decode every image of the run up front. Swapping a src mid-playback would
// blank the canvas for a frame, which in a tool built to find flicker would be
// indistinguishable from the thing being looked for.
function preload() {
  const names = viewNames();
  for (const frame of frames()) {
    for (const name of names) imageFor(frame.views[name]);
  }
}

function fillSelect(select, options, selected) {
  select.innerHTML = "";
  options.forEach((text, index) => {
    const option = document.createElement("option");
    option.value = String(index);
    option.textContent = text;
    if (index === selected) option.selected = true;
    select.appendChild(option);
  });
}

function buildViewButtons() {
  const holder = $("views");
  holder.innerHTML = "";
  viewNames().forEach((name, index) => {
    const button = document.createElement("button");
    button.textContent = (index + 1) + " " + name;
    button.setAttribute("aria-pressed", String(index === state.view));
    button.addEventListener("click", () => { state.view = index; render(); });
    holder.appendChild(button);
  });
  fillSelect($("blinkView"), viewNames(), state.blinkView);
}

function activeViewName() {
  const names = viewNames();
  if (state.blinking && state.blinkPhase) return names[state.blinkView] || names[0];
  return names[state.view] || names[0];
}

function drawSpark() {
  const canvas = $("spark");
  const width = canvas.clientWidth || 280;
  const height = 64;
  canvas.width = width * devicePixelRatio;
  canvas.height = height * devicePixelRatio;
  const context = canvas.getContext("2d");
  context.scale(devicePixelRatio, devicePixelRatio);
  const style = getComputedStyle(document.body);
  const accent = style.getPropertyValue("--accent").trim() || "#58a6ff";
  const line = style.getPropertyValue("--line").trim() || "#2a303a";
  const muted = style.getPropertyValue("--muted").trim() || "#949cab";
  context.clearRect(0, 0, width, height);

  const series = frames().map((frame) => {
    let worst = 0;
    for (const metric of frame.metrics) worst = Math.max(worst, metric.step);
    return worst;
  });
  const peak = Math.max(...series, 0.0001);
  context.strokeStyle = line;
  context.beginPath();
  context.moveTo(0, height - 0.5);
  context.lineTo(width, height - 0.5);
  context.stroke();

  context.strokeStyle = accent;
  context.lineWidth = 1.5;
  context.beginPath();
  series.forEach((value, index) => {
    const x = series.length < 2 ? 0 : (index / (series.length - 1)) * (width - 2) + 1;
    const y = height - 2 - (value / peak) * (height - 6);
    index === 0 ? context.moveTo(x, y) : context.lineTo(x, y);
  });
  context.stroke();

  const x = series.length < 2 ? 0 : (state.frame / (series.length - 1)) * (width - 2) + 1;
  context.strokeStyle = muted;
  context.beginPath();
  context.moveTo(x, 0);
  context.lineTo(x, height);
  context.stroke();
  $("sparkLabel").textContent = "peak lane error-step " + peak.toFixed(2) + " luma";
}

function renderMetrics() {
  const body = $("metrics").querySelector("tbody");
  body.innerHTML = "";
  const frame = frames()[state.frame];
  if (!frame.metrics.length) {
    body.innerHTML = "<tr><td colspan=\"5\">no per-frame step for this sequence</td></tr>";
    return;
  }
  let worst = 0;
  for (const metric of frame.metrics) worst = Math.max(worst, metric.step);
  for (const metric of frame.metrics) {
    const row = document.createElement("tr");
    const hot = metric.step >= worst && worst > 0 ? " class=\"hot\"" : "";
    row.innerHTML =
      "<td>" + metric.name + "</td>" +
      "<td" + hot + ">" + metric.step.toFixed(2) + "</td>" +
      "<td>" + metric.xstep.toFixed(2) + "</td>" +
      "<td>" + metric.worst.toFixed(1) + "</td>" +
      "<td>" + metric.x + "," + metric.y + "</td>";
    body.appendChild(row);
  }
}

function renderSheet() {
  const sheet = $("sheet");
  sheet.innerHTML = "";
  const name = viewNames()[state.view];
  frames().forEach((frame, index) => {
    const figure = document.createElement("figure");
    figure.dataset.current = index === state.frame ? "1" : "0";
    const image = document.createElement("img");
    image.src = frame.views[name];
    image.loading = "lazy";
    const caption = document.createElement("figcaption");
    caption.textContent = index + "  " + frame.seconds.toFixed(4) + "s" +
      (frame.label ? "  " + frame.label : "");
    figure.appendChild(image);
    figure.appendChild(caption);
    figure.addEventListener("click", () => {
      state.frame = index;
      state.sheet = false;
      $("sheetToggle").setAttribute("aria-pressed", "false");
      render();
    });
    sheet.appendChild(figure);
  });
}

function render() {
  const frame = frames()[state.frame];
  const name = activeViewName();
  const image = imageFor(frame.views[name]);
  const canvas = $("canvas");
  const zoom = parseFloat($("zoom").value);

  const paint = () => {
    canvas.width = image.naturalWidth || 1;
    canvas.height = image.naturalHeight || 1;
    canvas.style.width = Math.round(canvas.width * zoom) + "px";
    canvas.style.height = Math.round(canvas.height * zoom) + "px";
    canvas.getContext("2d").drawImage(image, 0, 0);
  };
  image.complete ? paint() : image.addEventListener("load", paint, { once: true });

  $("frame").max = String(frames().length - 1);
  $("frame").value = String(state.frame);
  $("frameLabel").textContent =
    "frame " + state.frame + " / " + (frames().length - 1) +
    "   pair " + frame.pair +
    "   alpha " + frame.alpha.toFixed(3) +
    "   t " + frame.seconds.toFixed(4) + "s" +
    (frame.label ? "   " + frame.label : "") +
    "   view " + name;

  document.querySelectorAll("#views button").forEach((button, index) => {
    button.setAttribute("aria-pressed", String(index === state.view));
  });
  $("stage").style.display = state.sheet ? "none" : "";
  $("sheet").style.display = state.sheet ? "grid" : "none";
  if (state.sheet) renderSheet();
  renderMetrics();
  drawSpark();
}

function step(delta) {
  const count = frames().length;
  let next = state.frame + delta;
  if (next >= count) next = $("loop").getAttribute("aria-pressed") === "true" ? 0 : count - 1;
  if (next < 0) next = $("loop").getAttribute("aria-pressed") === "true" ? count - 1 : 0;
  state.frame = next;
  render();
}

let playTimer = null;
function updatePlayback() {
  if (playTimer) clearInterval(playTimer);
  playTimer = null;
  if (!state.playing) return;
  const fps = Math.max(1, Math.min(60, parseInt($("fps").value, 10) || 8));
  playTimer = setInterval(() => step(1), 1000 / fps);
}

let blinkTimer = null;
function updateBlink() {
  if (blinkTimer) clearInterval(blinkTimer);
  blinkTimer = null;
  state.blinkPhase = false;
  if (!state.blinking) { render(); return; }
  const period = Math.max(60, parseInt($("blinkMs").value, 10) || 320);
  blinkTimer = setInterval(() => { state.blinkPhase = !state.blinkPhase; render(); }, period);
}

function jumpToWorst() {
  let bestIndex = 0;
  let best = -1;
  frames().forEach((frame, index) => {
    for (const metric of frame.metrics) {
      if (metric.step > best) { best = metric.step; bestIndex = index; }
    }
  });
  state.frame = bestIndex;
  render();
}

function selectSequence(index) {
  state.sequence = index;
  state.frame = 0;
  state.view = 0;
  state.blinkView = Math.min(1, Math.max(0, viewNames().length - 1));
  buildViewButtons();
  preload();
  render();
}

fillSelect($("sequence"), sequences.map((sequence) =>
  sequence.name + " (" + sequence.frames.length + ")"), 0);
$("sequence").addEventListener("change", (event) =>
  selectSequence(parseInt(event.target.value, 10)));
$("frame").addEventListener("input", (event) => {
  state.frame = parseInt(event.target.value, 10);
  render();
});
$("play").addEventListener("click", () => {
  state.playing = !state.playing;
  $("play").setAttribute("aria-pressed", String(state.playing));
  $("play").textContent = state.playing ? "pause" : "play";
  updatePlayback();
});
$("stepBack").addEventListener("click", () => step(-1));
$("stepForward").addEventListener("click", () => step(1));
$("fps").addEventListener("change", updatePlayback);
$("loop").addEventListener("click", () => {
  const on = $("loop").getAttribute("aria-pressed") === "true";
  $("loop").setAttribute("aria-pressed", String(!on));
});
$("zoom").addEventListener("change", render);
$("sheetToggle").addEventListener("click", () => {
  state.sheet = !state.sheet;
  $("sheetToggle").setAttribute("aria-pressed", String(state.sheet));
  render();
});
$("blink").addEventListener("click", () => {
  state.blinking = !state.blinking;
  $("blink").setAttribute("aria-pressed", String(state.blinking));
  updateBlink();
});
$("blinkView").addEventListener("change", (event) => {
  state.blinkView = parseInt(event.target.value, 10);
});
$("blinkMs").addEventListener("change", updateBlink);

// Pixel readout. Reading back a canvas drawn from a file:// image is blocked as
// a cross-origin read, so this works in the embedded viewer and quietly does
// nothing in the one that references PNGs on disk.
$("canvas").addEventListener("mousemove", (event) => {
  const canvas = $("canvas");
  const rect = canvas.getBoundingClientRect();
  const x = Math.floor((event.clientX - rect.left) / rect.width * canvas.width);
  const y = Math.floor((event.clientY - rect.top) / rect.height * canvas.height);
  let text = "x " + x + "  y " + y;
  try {
    const data = canvas.getContext("2d").getImageData(x, y, 1, 1).data;
    text += "   rgb " + data[0] + " " + data[1] + " " + data[2];
  } catch (error) { /* tainted canvas: coordinates only */ }
  $("readout").textContent = text;
});

document.addEventListener("keydown", (event) => {
  if (event.target.tagName === "INPUT" || event.target.tagName === "SELECT") return;
  if (event.key === "ArrowRight") { step(1); event.preventDefault(); }
  else if (event.key === "ArrowLeft") { step(-1); event.preventDefault(); }
  else if (event.key === " ") { $("play").click(); event.preventDefault(); }
  else if (event.key === "b") $("blink").click();
  else if (event.key === "g") $("sheetToggle").click();
  else if (event.key === "w") jumpToWorst();
  else if (event.key >= "1" && event.key <= "9") {
    const index = parseInt(event.key, 10) - 1;
    if (index < viewNames().length) { state.view = index; render(); }
  }
});

$("subtitle").textContent = DATA.subtitle;
selectSequence(0);
window.addEventListener("resize", drawSpark);
</script>
)HTML";

} // namespace

std::vector<double> LumaError(
    const std::span<const std::uint32_t> observed,
    const std::span<const std::uint32_t> expected) {
    const std::size_t count = std::min(observed.size(), expected.size());
    std::vector<double> error(count, 0.0);
    for (std::size_t index = 0; index < count; ++index) {
        error[index] = LumaOf(observed[index]) - LumaOf(expected[index]);
    }
    return error;
}

std::vector<std::uint32_t> RenderErrorView(
    const std::span<const double> error,
    const double gain) {
    std::vector<std::uint32_t> pixels(error.size(), 0xFF000000U);
    for (std::size_t index = 0; index < error.size(); ++index) {
        const double scaled = std::clamp(error[index] * gain, -255.0, 255.0);
        pixels[index] = scaled >= 0.0 ? Pack(scaled, scaled * 0.15, 0.0)
                                      : Pack(0.0, -scaled * 0.25, -scaled);
    }
    return pixels;
}

std::vector<std::uint32_t> RenderErrorStepView(
    const std::span<const double> error,
    const std::span<const double> previous_error,
    const double gain) {
    std::vector<std::uint32_t> pixels(error.size(), 0xFF000000U);
    if (previous_error.size() != error.size()) {
        return pixels;
    }
    for (std::size_t index = 0; index < error.size(); ++index) {
        const double magnitude = std::abs(error[index] - previous_error[index]) * gain;
        const double t = std::clamp(magnitude / 255.0, 0.0, 1.0);
        // Black -> red -> yellow -> white. Three legs rather than a single
        // brightness ramp so that a small step and a large one are different
        // hues, not two shades of grey a screenshot cannot separate.
        if (t < 1.0 / 3.0) {
            pixels[index] = Pack(t * 3.0 * 255.0, 0.0, 0.0);
        } else if (t < 2.0 / 3.0) {
            pixels[index] = Pack(255.0, (t - 1.0 / 3.0) * 3.0 * 255.0, 0.0);
        } else {
            pixels[index] = Pack(255.0, 255.0, (t - 2.0 / 3.0) * 3.0 * 255.0);
        }
    }
    return pixels;
}

FrameSequenceWriter::FrameSequenceWriter(
    std::filesystem::path directory,
    const std::uint32_t embed_divisor)
    : directory_(std::move(directory)), embed_divisor_(embed_divisor) {
    std::filesystem::create_directories(directory_);
}

bool FrameSequenceWriter::AddFrame(
    const std::string& sequence,
    const SequenceFrame& frame,
    const std::vector<SequenceView>& views,
    const std::uint32_t width,
    const std::uint32_t height,
    std::string& error) {
    auto found = std::find_if(
        sequences_.begin(),
        sequences_.end(),
        [&](const Sequence& candidate) { return candidate.name == sequence; });
    if (found == sequences_.end()) {
        sequences_.push_back(Sequence{sequence, {}});
        found = std::prev(sequences_.end());
        std::filesystem::create_directories(directory_ / sequence);
    }

    Record record;
    record.frame = frame;
    record.width = width;
    record.height = height;
    for (const SequenceView& view : views) {
        const std::string file =
            sequence + "-" + Padded(frame.index) + "-" + view.name + ".png";
        const std::filesystem::path path = directory_ / sequence / file;
        if (!WritePng(path, view.pixels, width, height, error)) {
            return false;
        }
        record.view_names.push_back(view.name);
        record.files.push_back(sequence + "/" + file);
        std::error_code status;
        const auto size = std::filesystem::file_size(path, status);
        if (!status) {
            bytes_written_ += size;
        }
        ++image_count_;

        if (embed_divisor_ > 0) {
            std::uint32_t scaled_width = width;
            std::uint32_t scaled_height = height;
            const auto scaled = DownscalePixels(
                view.pixels, width, height, embed_divisor_, scaled_width, scaled_height);
            record.embedded.push_back(
                "data:image/png;base64," +
                Base64Encode(EncodePng(scaled, scaled_width, scaled_height)));
        }
    }
    found->records.push_back(std::move(record));
    ++frame_count_;
    error.clear();
    return true;
}

std::string FrameSequenceWriter::BuildViewerHtml(
    const std::string& title,
    const bool embedded) const {
    std::ostringstream json;
    json << "{\"title\":\"" << JsonEscape(title) << "\",\"subtitle\":\""
         << JsonEscape(
                std::to_string(frame_count_) + " frames, " + std::to_string(image_count_) +
                " images" + (embedded ? ", inlined" : ", on disk beside this file"))
         << "\",\"sequences\":[";
    bool first_sequence = true;
    for (const Sequence& sequence : sequences_) {
        if (!first_sequence) {
            json << ',';
        }
        first_sequence = false;
        json << "{\"name\":\"" << JsonEscape(sequence.name) << "\",\"frames\":[";
        bool first_frame = true;
        for (const Record& record : sequence.records) {
            if (!first_frame) {
                json << ',';
            }
            first_frame = false;
            json << "{\"i\":" << record.frame.index << ",\"pair\":" << record.frame.pair
                 << ",\"seconds\":" << Number(record.frame.seconds, 6)
                 << ",\"alpha\":" << Number(record.frame.alpha, 4) << ",\"label\":\""
                 << JsonEscape(record.frame.label) << "\",\"views\":{";
            for (std::size_t index = 0; index < record.view_names.size(); ++index) {
                if (index > 0) {
                    json << ',';
                }
                const std::string& source = embedded && index < record.embedded.size()
                    ? record.embedded[index]
                    : record.files[index];
                json << '"' << JsonEscape(record.view_names[index]) << "\":\""
                     << JsonEscape(source) << '"';
            }
            json << "},\"metrics\":[";
            for (std::size_t index = 0; index < record.frame.metrics.size(); ++index) {
                const SequenceMetric& metric = record.frame.metrics[index];
                if (index > 0) {
                    json << ',';
                }
                json << "{\"name\":\"" << JsonEscape(metric.name)
                     << "\",\"step\":" << Number(metric.error_step)
                     << ",\"xstep\":" << Number(metric.crossfade_error_step)
                     << ",\"worst\":" << Number(metric.worst_error_step, 2)
                     << ",\"x\":" << metric.worst_x << ",\"y\":" << metric.worst_y << '}';
            }
            json << "]}";
        }
        json << "]}";
    }
    json << "]}";

    std::string html = kViewerHead;
    html += json.str();
    html += kViewerTail;
    return html;
}

bool FrameSequenceWriter::WriteViewer(const std::string& title, std::string& error) {
    if (sequences_.empty()) {
        error = "No frames were added, so there is nothing to view.";
        return false;
    }
    const auto write = [&](const std::filesystem::path& path, const std::string& html) {
        std::ofstream output(path, std::ios::binary);
        if (!output) {
            error = "Could not open " + path.string() + " for writing.";
            return false;
        }
        output.write(html.data(), static_cast<std::streamsize>(html.size()));
        if (!output) {
            error = "Writing " + path.string() + " failed.";
            return false;
        }
        bytes_written_ += html.size();
        return true;
    };

    if (!write(directory_ / "viewer.html", BuildViewerHtml(title, false))) {
        return false;
    }
    if (embed_divisor_ > 0 &&
        !write(directory_ / "viewer-embedded.html", BuildViewerHtml(title, true))) {
        return false;
    }
    error.clear();
    return true;
}

} // namespace osss
