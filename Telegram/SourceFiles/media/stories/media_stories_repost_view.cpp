/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "media/stories/media_stories_repost_view.h"

#include "core/ui_integration.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_stories.h"
#include "history/view/history_view_reply.h"
#include "main/main_session.h"
#include "media/stories/media_stories_controller.h"
#include "ui/effects/ripple_animation.h"
#include "ui/text/text_custom_emoji.h"
#include "ui/text/text_options.h"
#include "ui/painter.h"
#include "ui/power_saving.h"
#include "styles/style_chat.h"
#include "styles/style_media_view.h"

namespace Media::Stories {

RepostView::RepostView(
	not_null<Controller*> controller,
	not_null<Data::Story*> story)
: _controller(controller)
, _story(story) {
	Expects(_story->repost());

	_story->session().colorIndicesValue(
	) | rpl::start_with_next([=](Ui::ColorIndicesCompressed &&indices) {
		_colorIndices = std::move(indices);
		if (_maxWidth) {
			_controller->repaint();
		}
	}, _lifetime);
}

RepostView::~RepostView() = default;

int RepostView::height() const {
	return st::historyReplyPadding.top()
		+ st::semiboldFont->height
		+ st::normalFont->height
		+ st::historyReplyPadding.bottom();
}

void RepostView::draw(Painter &p, int x, int y, int availableWidth) {
	if (!_maxWidth) {
		recountDimensions();
	}
	if (_loading) {
		return;
	}
	const auto simple = _text.isEmpty();
	if (simple) {
		y += st::normalFont->height;
	}
	const auto w = std::min(int(_maxWidth), availableWidth);
	const auto h = height() - (simple ? st::normalFont->height : 0);
	const auto rect = QRect(x, y, w, h);
	const auto colorPeer = _story->repostSourcePeer();
	const auto backgroundEmojiId = (!simple && colorPeer)
		? colorPeer->backgroundEmojiId()
		: DocumentId();
	const auto cache = &_quoteCache;
	const auto &quoteSt = simple
		? st::storiesRepostSimpleStyle
		: st::messageQuoteStyle;
	const auto backgroundEmoji = backgroundEmojiId
		? &_backgroundEmojiData
		: nullptr;
	const auto backgroundEmojiCache = backgroundEmoji
		? &backgroundEmoji->caches[0]
		: nullptr;

	auto rippleColor = cache->bg;
	cache->bg = QColor(0, 0, 0, 64);
	Ui::Text::ValidateQuotePaintCache(*cache, quoteSt);
	Ui::Text::FillQuotePaint(p, rect, *cache, quoteSt);
	if (backgroundEmoji) {
		using namespace HistoryView;
		if (backgroundEmoji->firstFrameMask.isNull()
			&& !backgroundEmoji->emoji) {
			backgroundEmoji->emoji = CreateBackgroundEmojiInstance(
				&_story->owner(),
				backgroundEmojiId,
				crl::guard(this, [=] { _controller->repaint(); }));
		}
		ValidateBackgroundEmoji(
			backgroundEmojiId,
			backgroundEmoji,
			backgroundEmojiCache,
			cache);
		if (!backgroundEmojiCache->frames[0].isNull()) {
			const auto hasQuoteIcon = false;
			FillBackgroundEmoji(
				p,
				rect,
				hasQuoteIcon,
				*backgroundEmojiCache);
		}
	}
	cache->bg = rippleColor;

	if (_ripple) {
		_ripple->paint(p, x, y, w, &rippleColor);
		if (_ripple->empty()) {
			_ripple.reset();
		}
	}

	const auto pausedSpoiler = On(PowerSaving::kChatSpoiler);
	if (w > st::historyReplyPadding.left()) {
		const auto textw = w
			- st::historyReplyPadding.left()
			- st::historyReplyPadding.right();
		const auto namew = textw;
		if (namew > 0) {
			p.setPen(cache->icon);
			_name.drawLeftElided(
				p,
				x + st::historyReplyPadding.left(),
				y + st::historyReplyPadding.top(),
				namew,
				w + 2 * x);
			if (!simple) {
				const auto textLeft = x + st::historyReplyPadding.left();
				const auto textTop = y
					+ st::historyReplyPadding.top()
					+ st::semiboldFont->height;
				_text.draw(p, {
					.position = { textLeft, textTop },
					.availableWidth = textw,
					.palette = &st::mediaviewTextPalette,
					.spoiler = Ui::Text::DefaultSpoilerCache(),
					.pausedEmoji = On(PowerSaving::kEmojiChat),
					.pausedSpoiler = On(PowerSaving::kChatSpoiler),
					.elisionLines = 1,
				});
			}
		}
	}
}

void RepostView::recountDimensions() {
	const auto sender = _story->repostSourcePeer();
	const auto name = sender ? sender->name() : _story->repostSourceName();
	const auto owner = &_story->owner();
	const auto repostId = _story->repostSourceId();

	const auto colorIndexPlusOne = sender
		? (sender->colorIndex() + 1)
		: 1;
	const auto dark = true;
	const auto colorPattern = colorIndexPlusOne
		? Ui::ColorPatternIndex(_colorIndices, colorIndexPlusOne - 1, dark)
		: 0;
	Assert(colorPattern < Ui::Text::kMaxQuoteOutlines);
	const auto values = Ui::SimpleColorIndexValues(
		QColor(255, 255, 255),
		colorPattern);
	_quoteCache.bg = values.bg;
	_quoteCache.outlines = values.outlines;
	_quoteCache.icon = values.name;

	auto text = TextWithEntities();
	auto unavailable = false;
	if (sender && repostId) {
		const auto of = owner->stories().lookup({ sender->id, repostId });
		unavailable = !of && (of.error() == Data::NoStory::Deleted);
		if (of) {
			text = (*of)->caption();
		} else if (!unavailable) {
			const auto done = crl::guard(this, [=] {
				_maxWidth = 0;
				_controller->repaint();
			});
			owner->stories().resolve({ sender->id, repostId }, done);
		}
	}

	auto nameFull = TextWithEntities();
	nameFull.append(HistoryView::Reply::PeerEmoji(owner, sender));
	nameFull.append(name);
	auto context = Core::MarkedTextContext{
		.session = &_story->session(),
		.customEmojiRepaint = [] {},
		.customEmojiLoopLimit = 1,
	};
	_name.setMarkedText(
		st::semiboldTextStyle,
		nameFull,
		Ui::NameTextOptions(),
		context);
	context.customEmojiRepaint = crl::guard(this, [=] {
		_controller->repaint();
	}),
	_text.setMarkedText(
		st::defaultTextStyle,
		text,
		Ui::DialogTextOptions(),
		context);

	const auto nameMaxWidth = _name.maxWidth();
	const auto optimalTextWidth = _text.isEmpty()
		? 0
		: std::min(_text.maxWidth(), st::maxSignatureSize);
	_maxWidth = std::max(nameMaxWidth, optimalTextWidth);
	_maxWidth = st::historyReplyPadding.left()
		+ _maxWidth
		+ st::historyReplyPadding.right();
}

} // namespace Media::Stories