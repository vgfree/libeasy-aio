#pragma once

#define _GNU_SOURCE
#include <stdlib.h>
#include <assert.h>

/* a type safe version of qsort() */
#define xqsort(base, nmemb, compar)			     \
	({						     \
		if (nmemb > 1) {			     \
			qsort(base, nmemb, sizeof(*(base)),  \
			(comparison_fn_t)compar);	     \
			assert(compar(base, base + 1) <= 0); \
		}					     \
	})

/* a type safe version of bsearch() */
#define xbsearch(key, base, nmemb, compar)				   \
	({								   \
		typeof(&(base)[0])__ret = NULL;				   \
		if (nmemb > 0) {					   \
			assert(compar(key, key) == 0);			   \
			assert(compar(base, base) == 0);		   \
			__ret = bsearch(key, base, nmemb, sizeof(*(base)), \
			(comparison_fn_t)compar);			   \
		}							   \
		__ret;							   \
	})

/*
 * Binary Search of the ascending sorted array. When the key is not found, this
 * returns the next greater position.
 */
#define nbsearch(key, base, nmemb, compar)			    \
	({							    \
		typeof(key)__m, __l = base, __r = base + nmemb - 1; \
		int __ret;					    \
		while (__l <= __r && likely(nmemb > 0)) {	    \
			__m = __l + (__r - __l) / 2;		    \
			__ret = compar(key, __m);		    \
			if (__ret < 0) {			    \
				__r = __m - 1;			    \
			} else if (__ret > 0) {			    \
				__l = __m + 1;			    \
			} else {				    \
				__l = __m;			    \
				break;				    \
			}					    \
		}						    \
		__l;						    \
	})

/* a type safe version of lfind() */
#define xlfind(key, base, nmemb, compar)				\
	({								\
		typeof(&(base)[0])__ret = NULL;				\
		if (nmemb > 0) {					\
			size_t __n = nmemb;				\
			assert(compar(key, key) == 0);			\
			assert(compar(base, base) == 0);		\
			__ret = lfind(key, base, &__n, sizeof(*(base)),	\
			(comparison_fn_t)compar);			\
		}							\
		__ret;							\
	})

/*
 * Search 'key' in the array 'base' linearly and remove it if it found.
 *
 * If 'key' is found in 'base', this function increments *nmemb and returns
 * true.
 */
#define xlremove(key, base, nmemb, compar)				\
	({								\
		bool __removed = false;					\
		typeof(&(base)[0])__e;					\
									\
		__e = xlfind(key, base, *(nmemb), compar);		\
		if (__e != NULL) {					\
			(*(nmemb))--;					\
			memmove(__e, __e + 1,				\
			sizeof(*(base)) * (*(nmemb) - (__e - (base))));	\
			__removed = true;				\
		}							\
		__removed;						\
	})

static inline void xshuffle(void *base, size_t nel, size_t width)
{
	char tmp[width];

	int x = nel / 2;
	while (x--) {
		int i = (rand() % nel) * width;
		int j = (rand() % nel) * width;
		if (i == j) {
			x++;
			continue;
		}
		char *ptr1 = (char *)base + i;
		char *ptr2 = (char *)base + j;

		memcpy(tmp, ptr1, width);
		memcpy(ptr1, ptr2, width);
		memcpy(ptr2, tmp, width);
	}
}
