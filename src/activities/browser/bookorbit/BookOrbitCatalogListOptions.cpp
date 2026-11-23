#include "BookOrbitCatalogListOptions.h"

#include <I18n.h>
#include <Logging.h>

#include <iterator>
#include <utility>

#include "CrossPointSettings.h"
#include "components/OptionPopup.h"

namespace BookOrbitCatalogListOptions {
namespace {

// The Library's sort methods that BookOrbit can apply, in the Library's order. The server
// sorts authors by last name and has no first-name or genre order.
struct SortChoice {
  const char* id;  // BookOrbit's sort id
  StrId label;
  bool descendingFirst;  // the server's own direction for it: newest first for dates
};
constexpr SortChoice SORT_CHOICES[] = {
    {"recently_added", StrId::STR_LIBRARY_DATE_ADDED, true},
    {"title", StrId::STR_LIBRARY_TITLE, false},
    {"author", StrId::STR_LIBRARY_AUTHOR_LAST_NAME, false},
    {"recently_read", StrId::STR_BOOKORBIT_SORT_RECENTLY_READ, true},
    {"series", StrId::STR_LIBRARY_SERIES, false},
};
constexpr int SORT_CHOICE_COUNT = static_cast<int>(std::size(SORT_CHOICES));
// The sort picker's rows: the direction and the list view above a divider, then the methods.
constexpr int SORT_ROW_DIRECTION = 0;
constexpr int SORT_ROW_LIST_VIEW = 1;
constexpr int SORT_ROW_FIRST_METHOD = 2;
// What the server sorts by when a query names no sort (an author's books).
constexpr int DEFAULT_SORT_CHOICE = 0;

struct ReadStatusChoice {
  const char* id;  // BookOrbit's readStatus filter; empty for every book
  StrId label;
};
constexpr ReadStatusChoice READ_STATUS_CHOICES[] = {
    {"", StrId::STR_BOOKORBIT_STATUS_ALL},
    {"unread", StrId::STR_BOOKORBIT_STATUS_UNREAD},
    {"reading", StrId::STR_BOOKORBIT_STATUS_READING},
    {"finished", StrId::STR_BOOKORBIT_STATUS_FINISHED},
};
constexpr int READ_STATUS_CHOICE_COUNT = static_cast<int>(std::size(READ_STATUS_CHOICES));

int sortChoiceIndex(const std::string& sort) {
  if (sort.empty()) return DEFAULT_SORT_CHOICE;
  for (int i = 0; i < SORT_CHOICE_COUNT; i++) {
    if (sort == SORT_CHOICES[i].id) return i;
  }
  return DEFAULT_SORT_CHOICE;
}

int readStatusChoiceIndex(const std::string& readStatus) {
  for (int i = 0; i < READ_STATUS_CHOICE_COUNT; i++) {
    if (readStatus == READ_STATUS_CHOICES[i].id) return i;
  }
  return 0;
}
}  // namespace

bool sortsDescending(const BookOrbitBookQuery& query) {
  if (!query.order.empty()) return query.order == "desc";
  return SORT_CHOICES[sortChoiceIndex(query.sort)].descendingFirst;
}

const char* readStatusLabel(const std::string& readStatus) {
  return I18N.get(READ_STATUS_CHOICES[readStatusChoiceIndex(readStatus)].label);
}

void show(OptionPopup& popup, const Picker picker, const BookOrbitBookQuery& query, std::function<void(int)> onPick) {
  if (picker == Picker::Sort) {
    // The Library's button layout: the direction first, then the methods. The list view
    // rides along, so the sections can be turned off without leaving the catalog.
    const std::string listView = std::string(tr(STR_LIBRARY_LIST_VIEW)) + ": " +
                                 (SETTINGS.libraryListExpanded ? tr(STR_LIBRARY_EXPANDED) : tr(STR_COMPACT));
    const char* labels[SORT_ROW_FIRST_METHOD + SORT_CHOICE_COUNT];
    labels[SORT_ROW_DIRECTION] = sortsDescending(query) ? "Z-A" : "A-Z";
    labels[SORT_ROW_LIST_VIEW] = listView.c_str();
    for (int i = 0; i < SORT_CHOICE_COUNT; i++) labels[SORT_ROW_FIRST_METHOD + i] = I18N.get(SORT_CHOICES[i].label);
    popup.show(tr(STR_LIBRARY_SORT_BY), labels, SORT_ROW_FIRST_METHOD + SORT_CHOICE_COUNT,
               SORT_ROW_FIRST_METHOD + sortChoiceIndex(query.sort), std::move(onPick));
    popup.setDividerAfterOption(SORT_ROW_FIRST_METHOD - 1);
  } else {
    StrId labels[READ_STATUS_CHOICE_COUNT];
    for (int i = 0; i < READ_STATUS_CHOICE_COUNT; i++) labels[i] = READ_STATUS_CHOICES[i].label;
    popup.show(StrId::STR_BOOKORBIT_READ_STATUS, labels, READ_STATUS_CHOICE_COUNT,
               readStatusChoiceIndex(query.readStatus), std::move(onPick));
  }
  popup.setDismissOnOutsideTouchDown(true);
}

Change apply(const Picker picker, const int choice, const BookOrbitBookQuery& current, BookOrbitBookQuery& next) {
  next = current;
  if (picker == Picker::Filter) {
    if (choice >= READ_STATUS_CHOICE_COUNT) return Change::None;
    next.readStatus = READ_STATUS_CHOICES[choice].id;
  } else if (choice == SORT_ROW_LIST_VIEW) {
    return Change::ListView;
  } else if (choice == SORT_ROW_DIRECTION) {
    // The direction row. Spelled out only where it departs from the sort's own, so
    // the query, and the cache key built from it, stay the same for the same order.
    const bool descending = !sortsDescending(next);
    next.order = descending == SORT_CHOICES[sortChoiceIndex(next.sort)].descendingFirst ? ""
                 : descending                                                           ? "desc"
                                                                                        : "asc";
  } else {
    const int method = choice - SORT_ROW_FIRST_METHOD;
    // The current method keeps its direction; another starts in its own, as in the Library.
    if (method >= SORT_CHOICE_COUNT || method == sortChoiceIndex(next.sort)) return Change::None;
    next.sort = SORT_CHOICES[method].id;
    next.order.clear();
  }
  if (next.sort == current.sort && next.order == current.order && next.readStatus == current.readStatus) {
    return Change::None;
  }
  LOG_INF("BookOrbit", "Listing now sorted by %s%s%s, read status '%s'",
          next.sort.empty() ? SORT_CHOICES[DEFAULT_SORT_CHOICE].id : next.sort.c_str(), next.order.empty() ? "" : " ",
          next.order.c_str(), next.readStatus.c_str());
  return Change::Listing;
}

}  // namespace BookOrbitCatalogListOptions
