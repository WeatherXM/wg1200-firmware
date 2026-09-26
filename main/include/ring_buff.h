#ifndef RING_BUFF_H
#define RING_BUFF_H

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include "esp_log.h"
#include "ring_buff.h"

/******************************************************************************
* Ring buffer
* User is responsible for locking/unlocking mutex object using provided funcs.
******************************************************************************/

template <typename TStruct, int TSize>
class RingBuff
{
public:
	RingBuff();
	~RingBuff();

	void push(TStruct *el);
	bool pop(TStruct *out);
	bool pop_back(TStruct *out);

	bool peek(TStruct *out, int index);
	bool peek_oldest(TStruct *out);
	bool peek_newest(TStruct *out);

	void clear();

	int get_size() const;
	int get_item_count() const;

	void lock(uint32_t timeout = portMAX_DELAY) const;
	void unlock() const;

	void foreach(void cb(void*)) const;
private:
	/** Element buffer */
	TStruct _buff[TSize];

	/** Buffer head */
	TStruct *_head = _buff;

	/** Buffer tail */
	TStruct *_tail = _buff;

	/** Count of items currently in array */
	int _item_count = 0;

	/** Mutex object for buffer */
	SemaphoreHandle_t _sem_buffer;
	/** Semaphore buffer */
	StaticSemaphore_t _sem_buffer_buff;
};

/******************************************************************************
 * Constructor
 ******************************************************************************/
template <typename TStruct, int TSize>
RingBuff<TStruct, TSize>::RingBuff()
{
	// Create mutex to lock buffers
	_sem_buffer = xSemaphoreCreateMutexStatic(&_sem_buffer_buff);
};

/******************************************************************************
 * Destructor
 ******************************************************************************/
template <typename TStruct, int TSize>
RingBuff<TStruct, TSize>::~RingBuff(){};

/******************************************************************************
 * Push to head
 * @param el Item to push
 ******************************************************************************/
template <typename TStruct, int TSize>
void RingBuff<TStruct, TSize>::push(TStruct *el)
{
	// On init, tail and head point to the same item
	if (_item_count > 0)
	{
		// Reached end, wrap around
		if (_head == (_buff + (TSize - 1)))
		{
			_head = _buff;
		}
		else
		{
			_head++;
		}

		// Head and tail met, move tail and overwrite oldest value
		if (_head == _tail)
		{
			// Advance tail and wrap around if needed
			if (_tail == (_buff + (TSize - 1)))
			{
				_tail = _buff;
			}
			else
			{
				_tail++;
			}
		}
		else
		{
			// Advance counter only if no value was overwritten
			_item_count++;
		}
	}
	else
	{
		_item_count++;
	}

	// Copy element to head and advance count
	memcpy(_head, el, sizeof(TStruct));
}

/******************************************************************************
 * Pop from tail
 * @param out Pointer to output item
 * @return false when empty
 ******************************************************************************/
template <typename TStruct, int TSize>
bool RingBuff<TStruct, TSize>::pop(TStruct *out)
{
	if (_item_count <= 0)
	{
		return false;
	}

	// Copy element from tail
	memcpy(out, _tail, sizeof(TStruct));

	//if count == 1 you dont need to move the tail
	if (_item_count > 1)
	{
		// Advance tail and wrap around if needed
		if (_tail == (_buff + (TSize - 1)))
		{
			_tail = _buff;
		}
		else
		{
			_tail++;
		}
	}

	_item_count--;

	return true;
}

/******************************************************************************
 * Peek at item without removing it.
 * Item is still copied to output structure (no direct access to the buff)
 * @param out Pointer to output item
 * @return false when empty
 ******************************************************************************/
template <typename TStruct, int TSize>
bool RingBuff<TStruct, TSize>::peek(TStruct *out, int index)
{
	if(_item_count == 0)
	{
		return false;
	}

	if(index < 0 || index + 1 > _item_count)
	{
		return false;
	}

	memcpy(out, _buff[index], sizeof(TStruct));

	return true;
}

/******************************************************************************
 * Peek at oldest item in buff (tail)
 * Item is still copied to output structure (no direct access to the buff)
 * @param out Pointer to output item
 * @return false when empty
 ******************************************************************************/
template <typename TStruct, int TSize>
bool RingBuff<TStruct, TSize>::peek_oldest(TStruct *out)
{
	if(_item_count == 0)
	{
		return false;
	}

	memcpy(out, _tail, sizeof(TStruct));

	return true;
}

/******************************************************************************
 * Peek at news item in buff (head)
 * Item is still copied to output structure (no direct access to the buff)
 * @param out Pointer to output item
 * @return false when empty
 ******************************************************************************/
template <typename TStruct, int TSize>
bool RingBuff<TStruct, TSize>::peek_newest(TStruct *out)
{
	if(_item_count == 0)
	{
		return false;
	}

	memcpy(out, _head, sizeof(TStruct));

	return true;
}

/******************************************************************************
 * Number of items currently in buff
 * @return Number of items
 ******************************************************************************/
template <typename TStruct, int TSize>
int RingBuff<TStruct, TSize>::get_item_count() const
{
	return _item_count;
}

/******************************************************************************
 * Buffer TSize
 * @return Size of buffer (number of items)
 ******************************************************************************/
template <typename TStruct, int TSize>
int RingBuff<TStruct, TSize>::get_size() const
{
	return TSize;
}

/******************************************************************************
 * Clear buffer
 ******************************************************************************/
template <typename TStruct, int TSize>
void RingBuff<TStruct, TSize>::clear()
{
	_head = _buff;
	_tail = _buff;
	_item_count = 0;
}

/******************************************************************************
 * Lock FreeRTOS mutex
 ******************************************************************************/
template <typename TStruct, int TSize>
void RingBuff<TStruct, TSize>::lock(uint32_t timeout) const
{
	xSemaphoreTake(_sem_buffer, timeout);
}

/******************************************************************************
 * Unlock FreeRTOS mutex
 ******************************************************************************/
template <typename TStruct, int TSize>
void RingBuff<TStruct, TSize>::unlock() const
{
	xSemaphoreGive(_sem_buffer);
}

/******************************************************************************
* Iterate and peek through all buffer contents
******************************************************************************/
template <typename TStruct, int TSize>
void RingBuff<TStruct, TSize>::foreach(void cb(void*)) const
{
	if(get_item_count() < 1)
		return;

	TStruct *pos = _head;
	while(1)
	{
		cb(pos);

		if(pos == _tail)
			break;
		if(pos == _buff)
			pos += TSize;

		pos--;
	}   
	
}

#endif